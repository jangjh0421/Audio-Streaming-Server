# Music Streaming System Assignment

## 1. Introduction

In this assignment, we will endeavor to develop a system of interconnected applications in one of the most often-used and fundamental configurations: a server and accompanying client(s) connected through sockets. This particular server will serve a small music (media file) library such that compatible clients will be able to stream the files on the server’s library, save the files locally, or both.

In essence, streaming means that files can be requested from the server, and they are sent progressively and intermittently in chunks. Your implementation will be able to support many different media file types, including wav, mp3, ogg, flac, etc. The clients will use a dynamic buffer to receive as many packets as are available, as they become available, before redirecting them to their appropriate destination.

## 2. Basic Outline

The majority of the design is provided to you in the starter code, and you are tasked with completing:

### Server-side processing of received requests and associated responses, for:
- Listing all files
- Streaming particular files
- Setting up the server to listen on the specified port number

### Client-side sending of requests and receiving responses, for:
- Creating requests to list or stream files
- Receiving files with dynamic buffering
- Storing files as they are received
- (Separately from local storing) Piping files to an accompanying audio (media) player process

The challenge in this assignment is ensuring that no bytes are lost, all bytes are transferred and received in the right order, and this is done without memory errors, as well as feeling comfortable working with an already developed, and somewhat larger codebase. Throughout the starter code there are TODOs that must be completed. The functions and structs defined in the three header files `as_client.h`, `as_server.h`, and `libas.h` should not change, but you may add to these headers as you need, and change the values of different macros.
