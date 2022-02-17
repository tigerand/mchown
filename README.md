
<!-- Copyright 2020-2022 Andrew Sharp andy@tigerand.com, All Rights Reserved -->

# mchown - multi-threaded, recursive chown-like program

##Usage
usage:
mchown [-h] <path> <uid> <gid>
	where path is the FQ path of the heirarchy to process, and
	u/gid is the user/group id to set as the new ownership of the
	files in that heirarchy
	-h	help message
	-n N	use a thread pool with N threads, which must be less
		than the calculated number of threads or it will be ignored


###Build
* use *debug* make target when switching between debug and non-debug versions<br>
 ```make debug```
* use *clean* target when switching between debug and non-debug versions<br>
 ```make clean```
* the program now attempts to up the number of open file descriptors to 100 per thread on its own, calculations show that should be enough
* the program now attempts to up the max stacksize to 8M per thread
* only changes regular files, directories, and symlinks (regardless of what they point to).  Does not mess with pipes, sockets or device nodes.
* must run as superuser
* the <b>-d</b> option toggles debug output.  so, if the program is compiled with debug output turned on, calling program with <b>-d</b> runs the program with no debug output.  with debug output on, operating on a directory with 70,000 files, the output can be a couple hundred thousand lines, so this avoids the overhead of writing that output and the operator having to store it somewhere.
* the <b>-n N</b> option allows you to set the number of threads in the thread pool to less than the number otherwise created (90% of online logical cores).  this useful for researching the optimal number of threads to use, but mostly for allowing multiple copies of the program to be run at the same time.  the program is designed to handle multiple heirarchies at the same time and run as a daemon and invoked through a message queue or a socket or something, but not until phase 2.
