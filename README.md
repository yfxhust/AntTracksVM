# AntTracksVM
AntTracks is a JVM capabable of efficient and accurate object tracing for java applications.

Object allocations and garbage collection can have a considerable impact on the performance of Java applications.
Without monitoring tools, such performance problems are hard to track down, and if such tools are applied, they often cause a significant overhead and tend to distort the behavior of the monitored application.
In the paper Accurate and Efficient Object Tracing for Java Applications we proposed a novel light-weight memory monitoring approach in which we trace allocations, deallocations and movements of objects using VM-specific knowledge.
Our approach allows us to reconstruct the heap for any point in time and to do offine analyses on both the heap and on the trace. The average run-time overhead is 4.68%, which seems to be fast enough for keeping tracing switched on even in production mode.
This site provides downloads of our virtual machine and the analysis tool as well as tutorials on how to use both of them.



Using the AntTracks VM, you can execute and trace any Java application.
The VM will track every allocation, i.e., the allocation site (including the calling frames for frequent allocations), the allocated type, the allocating thread, the object size, as well as a number of other properties as well as object movements throughout the heap, and object deaths, i.e., when they are reclaimed by the garbage collector.
The analysis tool can then process the trace offline and visualize the change of specific metrics throughout the progress of the application (e.g., memory usage per space over time, GC pauses over time) as well as the heap at a specific point in time.



This project is a cooperation of the Institute for System Software, the Christian Doppler Laboratory for Monitoring and Evolution of Very-large-scale Software Systems and Dynatrace Austria.


## Building
See http://openjdk.java.net/ for more information about the OpenJDK.

See ../README-builds.html for complete details on build machine requirements.

### Simple Build Instructions:

```
    cd make && gnumake
```

  The files that will be imported into the JDK build will be in the "build"
  directory.
