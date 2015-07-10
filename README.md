# lcmaps-plugins-process-tracking

![](https://api.travis-ci.org/lcmaps-plugins/lcmaps-plugins-process-tracking.svg?branch=master)

This LCMAPS plugin is meant to run after a mapping is established in a glexec invocation.
It will use the kernel's proc-connector interface to track all processes spawned by a
payload and kill them if they attempt to outlive the parent process.

