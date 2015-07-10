
#include "config.h"

#include <fcntl.h>
#include <sys/types.h>

#ifdef HAVE_UNORDERED_MAP
#include <unordered_map>
#include <unordered_set>
#else
#include <ext/hash_map>
#include <ext/hash_set>
#endif

#include <list>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <syslog.h>
#include <string>
#include <sstream>

#include "proc_keeper.h"

#pragma GCC visibility push(hidden)

#ifdef HAVE_UNORDERED_MAP
typedef std::unordered_map<pid_t, std::list<pid_t>, std::hash<pid_t>, std::equal_to<pid_t> > PidListMap;
typedef std::unordered_map<pid_t, pid_t, std::hash<pid_t>, std::equal_to<pid_t> > PidPidMap;
typedef std::unordered_map<pid_t, unsigned long, std::hash<pid_t>, std::equal_to<pid_t> > PidLUMap;
typedef std::unordered_set<pid_t, std::hash<pid_t>, std::equal_to<pid_t> > PidSet;
#else

struct eqpid {
    bool operator()(const pid_t pid1, const pid_t pid2) const {
        return pid1 == pid2;
    }
};

typedef __gnu_cxx::hash_map<pid_t, std::list<pid_t>, __gnu_cxx::hash<pid_t>, eqpid> PidListMap;
typedef __gnu_cxx::hash_map<pid_t, pid_t, __gnu_cxx::hash<pid_t>, eqpid> PidPidMap;
typedef __gnu_cxx::hash_map<pid_t, unsigned long, __gnu_cxx::hash<pid_t>, eqpid> PidLUMap;
typedef __gnu_cxx::hash_set<pid_t, __gnu_cxx::hash<pid_t>, eqpid> PidSet;
#endif

typedef std::list<pid_t> PidList;

void
measure_cpu(pid_t pid, unsigned long &utime, unsigned long &stime) {
    utime = 0;
    stime = 0;
    std::stringstream ss;
    ss << "/proc/" << pid << "/stat";
    FILE *file = fopen(ss.str().c_str(), "r");
    if (!file) return;
    unsigned long tmp_utime, tmp_stime;
    int ret = fscanf(file, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
        &tmp_utime, &tmp_stime);
    fclose(file);
    if (ret == 2) {
        utime = tmp_utime;
        stime = tmp_stime;
    }
}

class ProcessTree {

public:
    ProcessTree(pid_t watched, pid_t watched2) : 
        m_watched(watched),
        m_alt_watched(watched2),
        m_live_procs(1),
        m_started_shooting(false),
        m_dead_utime(0),
        m_dead_stime(0)
    {
        //syslog(LOG_NOTICE, "glexec.mon[%d:%d]: Started, target uid %d", getpid(), watched2, watched);
    }
    int fork(pid_t, pid_t);
    void usage();
    int exit(pid_t);
    int shoot_tree();
    void get_usage(long unsigned &utime, long unsigned &stime);
    inline int is_done();
    inline pid_t get_pid() {return m_watched;}

private:
    PidSet m_ignored_pids;
    PidListMap m_pid_map;
    PidPidMap m_pid_reverse;
    pid_t m_watched;
    pid_t m_alt_watched;
    unsigned int m_live_procs;
    bool m_started_shooting;
    inline int record_new(pid_t, pid_t);
    PidLUMap m_utime, m_stime;
    long unsigned m_dead_utime, m_dead_stime;
};

inline int ProcessTree::is_done() {
    return !m_live_procs;
}

inline int ProcessTree::record_new(pid_t parent_pid, pid_t child_pid) {
    //syslog(LOG_DEBUG, "FORK %d -> %d\n", parent_pid, child_pid);
    m_live_procs++;
    PidList pl;
    pl.push_back(child_pid);
    m_pid_map[parent_pid] = pl;
    m_pid_reverse[child_pid] = parent_pid;
    return 0;
}

int ProcessTree::fork(pid_t parent_pid, pid_t child_pid) {
    PidListMap::iterator it;
    PidPidMap::const_iterator it2;
    if (m_ignored_pids.find(parent_pid) != m_ignored_pids.end()) {
        return 0;
    } else if ((parent_pid != 1) && (it = m_pid_map.find(parent_pid)) != m_pid_map.end()) {
        //syslog(LOG_DEBUG, "FORK %d -> %d\n", parent_pid, child_pid);
        m_live_procs++;
        (it->second).push_back(child_pid);
        m_pid_reverse[child_pid] = parent_pid;
        if (m_started_shooting) {
            shoot_tree();
        }
    } else if ((it2 = m_pid_reverse.find(parent_pid)) != m_pid_reverse.end()) {
        record_new(parent_pid, child_pid);
        if (m_started_shooting) {
            shoot_tree();
        }
    } else if (parent_pid == m_watched) {
        record_new(parent_pid, child_pid);
    } else {
        m_ignored_pids.insert(parent_pid);
        m_ignored_pids.insert(child_pid);
    }
    return 0;
}

void ProcessTree::usage() {
    PidPidMap::const_iterator it;
    for (it = m_pid_reverse.begin(); it != m_pid_reverse.end(); ++it) {
        pid_t pid = it->first;
        long unsigned utime, stime;

        measure_cpu(pid, utime, stime);

        PidLUMap::const_iterator it2 = m_utime.find(pid);
        if (it2 == m_utime.end()) {
            m_utime[pid] = utime;
        } else {
            if (it2->second > utime) {
                m_dead_utime += it2->second;
                m_utime[pid] += utime;
            } else {
                m_utime[pid] = utime;
            }
        }
        it2 = m_stime.find(pid);
        if (it2 == m_stime.end()) {
            m_stime[pid] = stime;
        } else {
            if (it2->second > stime) {
                m_dead_stime += it2->second;
                m_stime[pid] = stime;
            } else {
                m_stime[pid] = stime;
            }
        }
    }
}

void ProcessTree::get_usage(unsigned long &utime, unsigned long &stime) {
    utime = m_dead_utime;
    stime = m_dead_stime;
    PidLUMap::const_iterator it;
    for (it = m_utime.begin(); it != m_utime.end(); ++it) {
        utime += it->second;
    }
    for (it = m_stime.begin(); it != m_stime.end(); ++it) {
        stime += it->second;
    }
    long hz = sysconf(_SC_CLK_TCK);
    utime /= hz;
    stime /= hz;
}

int ProcessTree::shoot_tree() {
    m_started_shooting = true;

    // Kill it all.
    PidPidMap::const_iterator it;
    // Check to see if there's children of this process.
    int body_count = 0;
    for (it = m_pid_reverse.begin(); it != m_pid_reverse.end(); ++it) {
        if (it->first == 1)
            continue;
        if ((kill(it->first, SIGKILL) == -1) && (errno != ESRCH)) {
            syslog(LOG_ERR, "FAILURE TO KILL %d: %d %s\n", it->first, errno, strerror(errno));
        }
        body_count ++;
    }
    if (body_count) {
        syslog(LOG_DEBUG, "Cleaned all processes associated with %d\n", m_watched);
    }
    long unsigned utime, stime;
    get_usage(utime, stime);
    syslog(LOG_NOTICE, "glexec.mon[%d#%d]: Terminated, CPU user %lu system %lu", getpid(), m_alt_watched, utime, stime);
    return body_count;
}

int ProcessTree::exit(pid_t pid) {
    PidListMap::iterator it;
    PidPidMap::iterator it2;
    // The head or watched process has died.  Start shooting
    if (pid == m_alt_watched) {
        shoot_tree();
        syslog(LOG_DEBUG, "EXIT %d (trigger process)\n", pid);
    }
    if (pid == m_watched) {
        shoot_tree();
        syslog(LOG_DEBUG, "EXIT %d (watched process)\n", pid);
        m_live_procs--;
    }
    if (m_ignored_pids.find(pid) != m_ignored_pids.end()) {
        m_ignored_pids.erase(pid);
        return 0;
    }
    int in_pid_map = false;
    if ((it = m_pid_map.find(pid)) != m_pid_map.end()) {
        in_pid_map = true;
        PidList::const_iterator it3;
        PidList &pl = it->second;
        // Check to see if there's children of this process.
        for (it3 = pl.begin(); it3 != pl.end(); ++it3) {
             // Re-parent the process to init.
             const pid_t child_pid = *it3;
             if ((it2 = m_pid_reverse.find(child_pid)) != m_pid_reverse.end()) {
                 //syslog(LOG_DEBUG, "DAEMON %d\n", child_pid);
                 it2->second = 1;
             }
        }
        m_pid_map.erase(pid);
    }
    if ((it2 = m_pid_reverse.find(pid)) == m_pid_reverse.end()) {
        if (in_pid_map) {
            //syslog(LOG_DEBUG, "EXIT %d\n", pid);
            if (pid != m_watched) {
                m_live_procs--;
            }
        }
    } else {
        pid_t parent = it2->second;
        //syslog(LOG_DEBUG, "EXIT %d PARENT %d\n", pid, parent);
        if ((it = m_pid_map.find(parent)) != m_pid_map.end()) {
            (it->second).remove(pid);
        }
        m_pid_reverse.erase(pid);
        if (pid != m_watched) {
            m_live_procs--;
        }
    }
    return 0;
}

ProcessTree *gTree;

int initialize(pid_t watch, pid_t alt_watch) {
    gTree = new ProcessTree(watch, alt_watch);
    return 0;
}

int is_done() {
    if (gTree) {
        return gTree->is_done();
    }
    return 1;
}

void finalize() {
    if (gTree) {
        if (!is_done()) {
            syslog(LOG_ERR, "ERROR: Finalizing without finishing killing the pid %d tree.\n", gTree->get_pid());
        }
        delete gTree;
    }
    gTree = NULL;
}

int processFork(pid_t parent_pid, pid_t child_pid) {
    return gTree->fork(parent_pid, child_pid);
}

int processExit(pid_t pid) {
    return gTree->exit(pid);
}

void processUsage() {
    gTree->usage();
}

#pragma GCC visibility pop

