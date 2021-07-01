#include <sys/socket.h>
#include <sys/stat.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#define BUFFER_SIZE_SMALL 1024
#define BUFFER_SIZE 4096

char *validCharacters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
char *logFileName = "";

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutexLog = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutexRead = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t freeWorker = PTHREAD_COND_INITIALIZER;
pthread_cond_t workToDo = PTHREAD_COND_INITIALIZER;
pthread_cond_t confirm = PTHREAD_COND_INITIALIZER;
pthread_cond_t allFree = PTHREAD_COND_INITIALIZER;
pthread_cond_t backToWork = PTHREAD_COND_INITIALIZER;

int activeWorkThreads = 0;
int totalThreads = 4;
int clientHolder = -1;
int LOGFILE = -1;
int offset = 0;
int toRead = 0;
int activeWait = 0;


typedef struct thread_arg_t
{
  int id;
  int client;
}ThreadArg;

int findOffset(char *requestType, char *fileName, int contentLength)
{
  char str[15];
  int total = 0;

  if (strcmp(requestType, "HEAD") == 0)
  {
    total += strlen("HEAD /") + strlen(fileName) + strlen(" length ");
    sprintf(str, "%d", contentLength);
    total += strlen(str);
    total += strlen("\n========\n");

    return total;
  }
  else
  {
    sprintf(str, "%d", contentLength);

    total = 0;
    total += strlen(requestType) + strlen(fileName) + strlen(str);
    total += 11;

    int full = contentLength / 20;
    int rem = contentLength % 20;

    total += 69 * full;

    if (rem != 0)
    {
      total += rem *3 + 9;
    }

    total += 9;
  }

  return total;
}

int findOffsetError(char *requestType, char *fileName, int code)
{
  int total = 0;
  total += strlen("FAIL: ");
  total += strlen(requestType) + 2 + strlen(fileName) + strlen(" HTTP/1.1 --- response ");

  char someStr[5];
  sprintf(someStr, "%d", code);
  total += strlen(someStr);

  total += 10;

  return total;
}

void writeLogLines(int pOffset, uint8_t *buff, int loggedBytes, int totalBytes, int isLast)
{
  char writeBuff[1500];
  strcpy(writeBuff, "");
  int pastByteOffset = 0;
  int readB = 0;
  int readHold = 0;
  int full = loggedBytes / 20;
  int rem = loggedBytes % 20;
  pastByteOffset += 69 * full;

  if (rem != 0)
  {
    pastByteOffset += rem *3 + 8;
  }

  for (int i = loggedBytes; i < totalBytes + loggedBytes; i++)
  {

    if (i % 20 == 0 && i + 19 < totalBytes + loggedBytes)
    {
      sprintf(writeBuff + strlen(writeBuff), "%08d %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",i,
      buff[i - loggedBytes], buff[i - loggedBytes + 1], buff[i - loggedBytes + 2], buff[i - loggedBytes + 3], buff[i - loggedBytes + 4], 
      buff[i - loggedBytes + 5],buff[i - loggedBytes + 6], buff[i - loggedBytes + 7], buff[i - loggedBytes + 8], buff[i - loggedBytes + 9], 
      buff[i - loggedBytes + 10], buff[i - loggedBytes + 11],buff[i - loggedBytes + 12], buff[i - loggedBytes + 13], buff[i - loggedBytes + 14],
      buff[i - loggedBytes + 15], buff[i - loggedBytes + 16], buff[i - loggedBytes + 17],buff[i - loggedBytes + 18], buff[i - loggedBytes + 19]);
      i += 19;
    }
    else
    {
      if (i % 20 == 0)
      {
        sprintf(writeBuff + strlen(writeBuff), "%08d", i);
      }

      sprintf(writeBuff + strlen(writeBuff), " %02x", buff[i - loggedBytes]);

      if (i % 20 == 19 || (i == (totalBytes + loggedBytes - 1) && isLast == 1))
      {
        sprintf(writeBuff + strlen(writeBuff), "\n");
      }
    }

    if (i == (totalBytes + loggedBytes - 1) && isLast == 1)
    {
      sprintf(writeBuff + strlen(writeBuff), "========\n");
    }

    if (i % 19 == 0 || i == (totalBytes + loggedBytes - 1))
    {
      readHold = readB;
      readB += pwrite(LOGFILE, writeBuff, strlen(writeBuff), pOffset + readHold + pastByteOffset);
      strcpy(writeBuff, "");
    }
  }
}

void createResponse(char *buff, int code, ssize_t contentLength)
{
  strcpy(buff, "HTTP/1.1 ");

  if (code == 200)
  {
    strcat(buff, "200 OK");
  }
  else if (code == 201)
  {
    strcat(buff, "201 Created");
  }
  else if (code == 400)
  {
    strcat(buff, "400 Bad Request");
  }
  else if (code == 403)
  {
    strcat(buff, "403 Forbidden");
  }
  else if (code == 404)
  {
    strcat(buff, "404 Not Found");
  }
  else
  {
    strcat(buff, "500 Internal Server Error");
  }

  char buff2[100];
  sprintf(buff2, "\r\nContent-Length: %zu\r\n\r\n", contentLength);
  strcat(buff, buff2);
}

//Checks if provided filename has characters from allowed 64 and less than 27 characters
int validateFile(char *fileName)
{
  if ((int) strlen(fileName) > 27)
  {
    return -1;
  }

  for (int i = 0; i < (int) strlen(fileName); i++)
  {
    int seen = 0;

    for (int j = 0; j < (int) strlen(validCharacters); j++)
    {
      if (fileName[i] == validCharacters[j])
      {
        seen = 1;
      }
    }

    if (seen == 0)
    {
      return -1;
    }
  }

  return 0;
}

//Divides the request up and saves the important values. Checks if the request is allowed.
int validateRequest(char *request, char *requestType, char *fileName, char *contentSizeString)
{
  char httpVersion[10];
  int read = sscanf(request, "%s /%s HTTP/%s", requestType, fileName, httpVersion);

  if (read < 3)
  {
    return -1;
  }

  if (strcmp(httpVersion, "1.1") != 0)
  {
    return -1;
  }

  if (strcmp(requestType, "PUT") == 0)
  {
    int count = 0;

    char *sentData1 = strstr(request, "Content-Length:");

    if (sentData1 == NULL)  //No content length for a put request
    {
      return -1;
    }

    for (int i = 16; i < (int) strlen(sentData1); i++)
    {
      if (sentData1[i] == '\r')
      {
        contentSizeString[count] = 0;
        break;
      }

      contentSizeString[count] = sentData1[i];
      count++;
    }
  }
  else
  {
    contentSizeString = "0";
  }

  char *saveptr;
  char *token = strtok_r(request, "\r\n", &saveptr);
  token = strtok_r(NULL, "\r\n", &saveptr);

  while (token != NULL)
  {
    int seen = 0;
    int countFront = 0;
    int countEnd = 0;

    for (int i = 0; i < (int) strlen(token); i++)
    {
      if (seen == 0)
      {
        countFront += 1;
      }

      if (seen == 1)
      {
        countEnd += 1;
      }

      if (token[i] == ':')
      {
        if (i + 1 < (int) strlen(token))
        {
          if (token[i + 1] == ' ')
          {
            seen = 1;
          }
        }
      }
    }

    if (seen == 0 || countFront < 2 || countEnd < 2)
    {
      return -1;
    }

    token = strtok_r(NULL, "\r\n", &saveptr);
  }

  if (validateFile(fileName) == -1)
  {
    return -1;
  }

  if (strcmp(requestType, "PUT") != 0 && strcmp(requestType, "GET") != 0 && strcmp(requestType, "HEAD") != 0)
  {
    return -1;
  }

  if (strcmp(requestType, "PUT") == 0 && strlen(requestType) > 3)
  {
    return -1;
  }

  if (strcmp(requestType, "GET") == 0 && strlen(requestType) > 3)
  {
    return -1;
  }

  if (strcmp(requestType, "HEAD") == 0 && strlen(requestType) > 4)
  {
    return -1;
  }

  return 0;
}

ssize_t getFileSize(char *filename)
{
  struct stat statStruct;
  int check = stat(filename, &statStruct);

  if (check == 0)
  {
    return statStruct.st_size;
  }

  return -1;
}

int checkPermission(char *filename, int permission)
{
  struct stat statStruct;
  stat(filename, &statStruct);

  if (permission == 0) //read
  {
    if (statStruct.st_mode &S_IRUSR)
    {
      return 0;
    }
    else
    {
      return -1;
    }
  }

  if (permission == 1) //write
  {
    if (statStruct.st_mode &S_IWUSR)
    {
      return 0;
    }
    else
    {
      return -1;
    }
  }

  return -1;
}

void writeFile(char *fileName, int clientSocket, uint8_t *buff, int sentSize, int bytes)
{
  int hasPerm = checkPermission(fileName, 1);

  char response[100];

  int fd = open(fileName, O_RDWR, 0);
  int errorNum = errno;

  if ((fd == -1 && errorNum == 13) || (fd != -1 && hasPerm == -1))  //Permission issue
  {
    createResponse(response, 403, 0);
    send(clientSocket, response, strlen(response), 0);

    if (LOGFILE != -1)
    {
      pthread_mutex_lock(&mutexLog);
      int currentOffset = offset; //offset is global
      int toReserve = findOffsetError("PUT", fileName, 403);
      offset += toReserve;
      pthread_mutex_unlock(&mutexLog);

      sprintf(response, "FAIL: PUT /%s HTTP/1.1 --- response 403\n========\n", fileName);
      pwrite(LOGFILE, response, strlen(response), currentOffset);
    }
  }
  else
  {
    close(fd);
    fd = creat(fileName, 0644);

    int readB;
    int currentOffset;
    int loggedBytes = 0;
    if (LOGFILE != -1)
    {
      pthread_mutex_lock(&mutexLog);
      currentOffset = offset; //offset is global
      int toReserve = findOffset("PUT", fileName, sentSize);
      offset += toReserve;
      pthread_mutex_unlock(&mutexLog);

      sprintf(response, "PUT /%s length %d\n", fileName, sentSize);
      readB = pwrite(LOGFILE, response, strlen(response), currentOffset);
    }

    int localSize = sentSize;
    write(fd, buff, bytes);
    localSize -= bytes;

    if (LOGFILE != -1)
    {
      if (localSize <= 0)
      {
        writeLogLines(readB + currentOffset, buff, loggedBytes, (int) bytes, 1);
      }
      else
      {
        writeLogLines(readB + currentOffset, buff, loggedBytes, (int) bytes, 0);
      }

      loggedBytes += (int) bytes;
    }

    int isClosed = 0;
    uint8_t buff2[BUFFER_SIZE + 1];

    while (localSize > 0)
    {
      ssize_t bytes2 = recv(clientSocket, buff2, BUFFER_SIZE, 0);

      if (bytes2 == 0)
      {
        isClosed = 1;
        break;
      }

      buff2[bytes2] = 0;

      write(fd, buff2, bytes2);
      localSize -= (int) bytes2;

      if (LOGFILE != -1)
      {
        if (localSize <= 0)
        {
          writeLogLines(readB + currentOffset, buff2, loggedBytes, (int) bytes2, 1);
        }
        else
        {
          writeLogLines(readB + currentOffset, buff2, loggedBytes, (int) bytes2, 0);
        }

        loggedBytes += (int) bytes2;
      }
    }

    if (LOGFILE != -1)
    {
      if (sentSize == 0)
      {
        sprintf(response, "========\n");
        pwrite(LOGFILE, response, strlen(response), currentOffset + readB);
      }
    }

    close(fd);

    if (isClosed == 1)
    {
      createResponse(response, 500, 0);
    }
    else
    {
      createResponse(response, 201, 0);
    }

    send(clientSocket, response, strlen(response), 0);
  }
}

void readFile(char *fileName, int clientSocket)
{
  int hasPerm = checkPermission(fileName, 0);

  int fd = open(fileName, O_RDONLY, 0);
  int errorNum = errno;

  char response[100];

  if (fd == -1 || (fd != -1 && hasPerm == -1))
  {
    int code;

    if (errorNum == 13 || (fd != -1 && hasPerm == -1))  //Permission issue
    {
      createResponse(response, 403, 0);
      send(clientSocket, response, strlen(response), 0);
      code = 403;
    }
    else if (errorNum == 2) //File does not exist
    {
      createResponse(response, 404, 0);
      send(clientSocket, response, strlen(response), 0);
      code = 404;
    }
    else  //default to
    {
      createResponse(response, 500, 0);
      send(clientSocket, response, strlen(response), 0);
      code = 500;
    }

    if (LOGFILE != -1)
    {
      pthread_mutex_lock(&mutexLog);
      int currentOffset = offset; //offset is global
      int toReserve = findOffsetError("GET", fileName, code);
      offset += toReserve;
      pthread_mutex_unlock(&mutexLog);

      sprintf(response, "FAIL: GET /%s HTTP/1.1 --- response %d\n========\n", fileName, code);
      pwrite(LOGFILE, response, strlen(response), currentOffset);
    }
  }
  else
  {
    ssize_t fileSize = getFileSize(fileName);
    createResponse(response, 200, fileSize);
    send(clientSocket, response, strlen(response), 0);

    int currentOffset;
    int readB;
    int loggedBytes = 0;

    if (LOGFILE != -1)
    {
      pthread_mutex_lock(&mutexLog);
      currentOffset = offset;
      int toReserve = findOffset("GET", fileName, (int) fileSize);
      offset += toReserve;
      pthread_mutex_unlock(&mutexLog);

      sprintf(response, "GET /%s length %d\n", fileName, (int) fileSize);
      readB = pwrite(LOGFILE, response, strlen(response), currentOffset);
    }

    uint8_t buff[BUFFER_SIZE + 1];
    ssize_t bytes;

    while ((bytes = read(fd, buff, BUFFER_SIZE)) > 0)
    {
      buff[bytes] = 0;
      write(clientSocket, buff, bytes);

      if (LOGFILE != -1)
      {
        if (bytes < BUFFER_SIZE)
        {
          writeLogLines(readB + currentOffset, buff, loggedBytes, (int) bytes, 1);
        }
        else
        {
          writeLogLines(readB + currentOffset, buff, loggedBytes, (int) bytes, 0);
        }

        loggedBytes += (int) bytes;
      }
    }

    if (LOGFILE != -1)
    {
      if (fileSize == 0)
      {
        sprintf(response, "========\n");
        pwrite(LOGFILE, response, strlen(response), currentOffset + readB);
      }
    }

    close(fd);
  }
}

void checkFile(char *fileName, int clientSocket)
{
  int hasPerm = checkPermission(fileName, 0);

  int fd = open(fileName, O_RDONLY, 0);
  int errorNum = errno;

  char response[100];

  if (fd == -1 || (fd != -1 && hasPerm == -1))
  {
    int code;

    if (errorNum == 13 || (fd != -1 && hasPerm == -1))  //Permission issue
    {
      createResponse(response, 403, 0);
      send(clientSocket, response, strlen(response), 0);
      code = 403;
    }
    else if (errorNum == 2) //File does not exist
    {
      createResponse(response, 404, 0);
      send(clientSocket, response, strlen(response), 0);
      code = 404;
    }
    else  //default to
    {
      createResponse(response, 500, 0);
      send(clientSocket, response, strlen(response), 0);
      code = 500;
    }

    if (LOGFILE != -1)
    {
      pthread_mutex_lock(&mutexLog);
      int currentOffset = offset;
      int toReserve = findOffsetError("HEAD", fileName, code);
      offset += toReserve;
      pthread_mutex_unlock(&mutexLog);

      sprintf(response, "FAIL: HEAD /%s HTTP/1.1 --- response %d\n========\n", fileName, code);
      pwrite(LOGFILE, response, strlen(response), currentOffset);
    }
  }
  else
  {
    ssize_t fileSize = getFileSize(fileName);
    createResponse(response, 200, fileSize);
    send(clientSocket, response, strlen(response), 0);

    if (LOGFILE != -1)
    {
      pthread_mutex_lock(&mutexLog);
      int currentOffset = offset; //offset is global
      int toReserve = findOffset("HEAD", fileName, (int) fileSize);
      offset += toReserve;
      pthread_mutex_unlock(&mutexLog);

      sprintf(response, "HEAD /%s length %d\n========\n", fileName, (int) fileSize);
      pwrite(LOGFILE, response, strlen(response), currentOffset);
    }

    close(fd);
  }
}

void performHealthCheck(int clientSocket)
{
  int errorCount = 0;
  int entries = 0;
  int equalCount = 0;
  int count = 0;
  int counter = 0;

  ssize_t bytes = 0;
  uint8_t buff[BUFFER_SIZE + 1];
  char str[5];
  pthread_mutex_lock(&mutexRead);
  toRead = 1;

  while (activeWorkThreads - activeWait > 1)
  {
    pthread_cond_wait(&allFree, &mutexRead);
  }

  ssize_t currentOffset = getFileSize(logFileName);
  int LOGFILERead = open(logFileName, O_RDONLY, 0);

  while (count < currentOffset)
  {
    if (currentOffset - BUFFER_SIZE - count >= 0)
    {
      bytes = pread(LOGFILERead, buff, BUFFER_SIZE, count);
      buff[bytes] = 0;
    }
    else
    {
      bytes = pread(LOGFILERead, buff, currentOffset - count, count);
      buff[bytes] = 0;
    }

    if (bytes <= 0)
    {
      break;
    }

    for (int i = 0; i < (int) bytes; i++)
    {
      if (count == 0 && i == 0)
      {
        str[0] = buff[0];
        str[1] = buff[1];
        str[2] = buff[2];
        str[3] = buff[3];
        str[4] = '\0';

        if (strcmp(str, "FAIL") == 0)
        {
          errorCount++;
        }
      }

      if (buff[i] == '=')
      {
        equalCount++;
      }

      if (equalCount == 8 && counter == 0)
      {
        i++;  //skip \n
        entries++;
        counter = 1;
      }
      else if (counter > 0)
      {
        str[counter - 1] = buff[i];
        counter++;

        if (counter >= 5)
        {
          str[counter - 1] = '\0';
          if (strcmp(str, "FAIL") == 0)
          {
            errorCount++;
          }
          counter = 0;
          equalCount = 0;
        }
      }
    }

    count += (int) bytes;
  }

  char response1[100];
  char response2[50];

  sprintf(response2, "%d\n%d", errorCount, entries);
  sprintf(response1, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n%s", (int) strlen(response2), response2);
  send(clientSocket, response1, strlen(response1), 0);

  close(LOGFILERead);

  pthread_mutex_lock(&mutexLog);
  int currentOffset2 = offset;
  int toReserve = findOffset("GET", "healthcheck", strlen(response2));
  offset += toReserve;
  pthread_mutex_unlock(&mutexLog);

  sprintf(response1, "GET /healthcheck length %d\n", (int) strlen(response2));
  int readB = pwrite(LOGFILE, response1, strlen(response1), currentOffset2);
  uint8_t tempBuff[100];

  for (int i = 0; i < (int) strlen(response2); i++)
  {
    tempBuff[i] = response2[i];
  }

  tempBuff[strlen(response2)] = 0;
  writeLogLines(currentOffset2 + readB, tempBuff, 0, (int) strlen(response2), 1);
  toRead = 0;
  pthread_cond_signal(&backToWork);
  pthread_mutex_unlock(&mutexRead);
}

void *worker(void *thr)
{
  ThreadArg *threadArg = (ThreadArg*) thr;

  while (true)
  {
    pthread_mutex_lock(&mutexRead);
    pthread_cond_signal(&allFree);
    if (toRead == 1)
    {
      activeWait++;
      pthread_cond_wait(&backToWork, &mutexRead);
      activeWait--;
    }
    pthread_cond_signal(&backToWork);
    pthread_mutex_unlock(&mutexRead);

    pthread_mutex_lock(&mutex);
    if (activeWorkThreads > 0)
    {
      activeWorkThreads--;
    }
    pthread_cond_signal(&freeWorker);
    pthread_cond_wait(&workToDo, &mutex);

    threadArg->client = clientHolder;
    activeWorkThreads++;
    pthread_cond_signal(&confirm);
    pthread_mutex_unlock(&mutex);

    pthread_mutex_lock(&mutexRead);
    pthread_cond_signal(&allFree);
    if (toRead == 1)
    {
      activeWait++;
      pthread_cond_wait(&backToWork, &mutexRead);
      activeWait--;
    }
    pthread_cond_signal(&backToWork);
    pthread_mutex_unlock(&mutexRead);

    uint8_t buff[BUFFER_SIZE + 1];
    ssize_t bytes = recv(threadArg->client, buff, BUFFER_SIZE, 0);
    buff[bytes] = 0;

    char requestType[100];
    char fileName[100];
    char contentSizeString[100];

    char request[BUFFER_SIZE + 1];
    strcpy(request, (char*) buff);
    char *sentData = strstr(request, "\r\n\r\n");
    sentData[0] = 0;

    int requestLength = (int) strlen(request) + 4;
    int validRequest = validateRequest(request, requestType, fileName, contentSizeString);
    char response[300];

    if (validRequest == -1)
    {
      createResponse(response, 400, 0);
      send(threadArg->client, response, strlen(response), 0);

      if (LOGFILE != -1)
      {
        strcpy(request, (char*) buff);
        sentData = strstr(request, "\r\n");
        sentData[0] = 0;
        sprintf(response, "FAIL: %s --- response 400\n========\n", request);

        pthread_mutex_lock(&mutexLog);
        int currentOffset = offset;
        int toReserve = strlen(response);
        offset += toReserve;
        pthread_mutex_unlock(&mutexLog);

        pwrite(LOGFILE, response, strlen(response), currentOffset);
      }
    }
    else
    {
      if (strcmp(requestType, "PUT") == 0)
      {
        if (strcmp(fileName, "healthcheck") == 0)
        {
          createResponse(response, 403, 0);
          send(threadArg->client, response, strlen(response), 0);

          if (LOGFILE != -1)
          {
            strcpy(request, (char*) buff);
            sentData = strstr(request, "\r\n");
            sentData[0] = 0;
            sprintf(response, "FAIL: %s --- response 403\n========\n", request);

            pthread_mutex_lock(&mutexLog);
            int currentOffset = offset;
            int toReserve = strlen(response);
            offset += toReserve;
            pthread_mutex_unlock(&mutexLog);

            pwrite(LOGFILE, response, strlen(response), currentOffset);
          }
        }
        else
        {
          int contentSize = atoi(contentSizeString);
          writeFile(fileName, threadArg->client, buff + requestLength, contentSize, (int) bytes - requestLength);
        }
      }
      else if (strcmp(requestType, "GET") == 0)
      {
        if (strcmp(fileName, "healthcheck") == 0)
        {
          if (LOGFILE != -1)
          {
            performHealthCheck(threadArg->client);
          }
          else
          {
            createResponse(response, 404, 0);
            send(threadArg->client, response, strlen(response), 0);
          }
        }
        else
        {
          readFile(fileName, threadArg->client);
        }
      }
      else 
      {
        if (strcmp(fileName, "healthcheck") == 0)
        {
          createResponse(response, 403, 0);
          send(threadArg->client, response, strlen(response), 0);

          if (LOGFILE != -1)
          {
            strcpy(request, (char*) buff);
            sentData = strstr(request, "\r\n");
            sentData[0] = 0;
            sprintf(response, "FAIL: %s --- response 403\n========\n", request);

            pthread_mutex_lock(&mutexLog);

            int currentOffset = offset;
            int toReserve = strlen(response);
            offset += toReserve;

            pthread_mutex_unlock(&mutexLog);

            pwrite(LOGFILE, response, strlen(response), currentOffset);
          }
        }
        else
        {
          checkFile(fileName, threadArg->client);
        }
      }
    }

    close(threadArg->client);
  }
}

void *dispatcher(void *thr)
{
  ThreadArg *threadArg = (ThreadArg*) thr;

  struct sockaddr client_addr;
  socklen_t client_addrlen = sizeof(client_addr);

  while (true)
  {

    int client_sockd = accept(threadArg->client, &client_addr, &client_addrlen);
    pthread_mutex_lock(&mutex);
    clientHolder = client_sockd;

    if (activeWorkThreads >= totalThreads - 1)
    {
      pthread_cond_wait(&freeWorker, &mutex);
    }

    pthread_cond_signal(&workToDo);
    pthread_cond_wait(&confirm, &mutex);
    pthread_mutex_unlock(&mutex);
  }
}

int main(int argc, char **argv)
{
  char *port = "-1";
  int logFile = -1;
  int numThreads = 5;
  int opt;

  //This really influenced my getopt design: https://www.geeksforgeeks.org/getopt-function-in-c-to-parse-command-line-arguments/
  while ((opt = getopt(argc, argv, "N:l:")) != -1)
  {
    switch (opt)
    {
      case 'l':
        logFile = creat(optarg, 0644);
        LOGFILE = logFile;
        logFileName = optarg;
        break;
      case 'N':
        numThreads = atoi(optarg) + 1;
        break;
    }
  }

  for (; optind < argc; optind++)
  {
    if (argc - optind == 1)
    {
      port = argv[optind];
    }
    else
    {
      break;
    }
  }

  if (strcmp(port, "-1") == 0)
  {
    return 1;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(atoi(port));
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  socklen_t addrlen = sizeof(server_addr);

  int server_sockd = socket(AF_INET, SOCK_STREAM, 0);

  if (server_sockd < 0)
  {
    perror("socket");
  }

  int enable = 1;
  int ret = setsockopt(server_sockd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  ret = bind(server_sockd, (struct sockaddr *) &server_addr, addrlen);
  ret = listen(server_sockd, SOMAXCONN);

  if (ret < 0)
  {
    return 1;
  }

  totalThreads = numThreads;
  pthread_t thread[numThreads];
  ThreadArg threadArgs[numThreads];

  for (int i = 1; i < numThreads; i++)
  {
    threadArgs[i].id = i;
    threadArgs[i].client = -1;
    pthread_create(&thread[i], NULL, worker, &threadArgs[i]);
  }

  threadArgs[0].id = 0;
  threadArgs[0].client = server_sockd;
  pthread_create(&thread[0], NULL, dispatcher, &threadArgs[0]);

  for (int i = 0; i < numThreads; i++)
  {
    pthread_join(thread[i], NULL);
  }

  close(server_sockd);

  return 0;
}