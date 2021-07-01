# HTTP server

The implementation of a multi-threaded HTTP server. The only requests that it will respond to are PUT, GET, and HEAD. The PUT is used to write files to the server. The GET is used to read files from the server. The HEAD is similar to GET but will only receive the content length and no file data. No files with above 27 characters in their name or that have a name with characters that are not the alphabet, digits, underscore, or dash will be accepted.

## Past Assignment
This server was an assignment from my principles of computer systems design class when I was an undergrad at UCSC.

## Executing program

Running make should result in a binary file called httpserver. It needs to be run with an argument specifying which port to start the server. Two optional arguments can be placed. -N followed by a positive integer will result in that many worker threads running in the server. -l followed by a file name will act like a log file and store its contents there. Now that the server is up it can receive PUT, GET, and HEAD requests.