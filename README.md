Aproxymate
==========

What is aproxymate?  It's a proxy, mate!  Or, at least, a close approximation of
one in around 560 lines of C.  *(I'm sorry, but after a while, you have to come
up with clever/stupid/punny GitHub repo names, or else it's all meaningless.)*

Anyway, the good old EECS 325 Networking students needed to implement a proxy in
Java.  I [did that](https://github.com/brenns10/proxy) earlier, and decided I
wanted a bit more of a challenge.  Thus, I'm doing it again, this time in C.

Which is pretty fun because it involves a staggering amount of non-trivial stuff
in C.  Like:
- Threading.  Lots of threading.
- Sockets (obviously)
- So much string handling
- Hash tables (for headers)
- Logging (well, when I get around to it)
- Memory management (although there must be memory leaks right now)
