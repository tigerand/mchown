## Design overview for the multi-threaded chown utility program

<!-- Copyright 2020-2022 Andrew Sharp andy@tigerand.com, All Rights Reserved
-->

<b>mchown</b> is a program that performs a multi-threaded, scan of a file system heirarchy and modifies the metadata of the files and directories to change the user and group ids of those files.  It does not modify sockets/pipes or device files.  The idea is to maximize speed of the operation as much as the computer resources of the system allow.

Some design objectives:

* Use about 90% of the logical cores in the CPU to traverse the filesystem.
* use a thread pool design to avoid the high cost of forking and reaping threads
* minimize the features in order to minizime the amount of locking
* fall back to single threaded recursion if no threads available
* \[daemon\] be able to process multiple different heirarchy/credential pairs simultaneously

```
 main directory processing function (mdpf)
    called with \{directory to process, cred, job id\}
    iterates through the directory entries:
        if file is a directory, if it is the first directory encountered, save it for processing outside the loop, otherwise attempt to queue it.  if that fails, then recursively call mdpf on it
        if file is a regular file or symlink, mod it
    if there is a saved dir, and if it was the only processable file in this directory, recurse into it, otherwise queue it

 queue processing function
    all worker threads sleep on queue cv
    threads wake up and take a task off queue and call mdpf

 enqueue function
    called to add a directory to the queue
    checks to see if any threads are available
    if yes
        add the dir to the queue
        bcast cv
    else
        return failure
    endif
```
