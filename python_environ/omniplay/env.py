import os
import re
import time
import shlex
import tempfile
import subprocess
import collections

LogCkpt = collections.namedtuple('LogCheckpoint',
        ['pid', 'group_id', 'parent_group_id', 'exe', 'args', 'env'])

def run_shell(cmd, stin=None, outp=None, err=None):
    outp_needs_close = False
    err_needs_close = False
    stin_needs_close = False
    if outp is None:
        outp_needs_close = True
        outp = open(os.devnull, 'w')

    if err is None:
        err_needs_close = True
        err = open(os.devnull, 'w')

    if stin is None:
        stin_needs_close = True
        stin = open(os.devnull, 'r')

    cmd = ''.join(['bash -c "', cmd, '"'])

    proc = subprocess.Popen(shlex.split(cmd), shell=False, stdin=stin, stdout=outp, stderr=err)
    if proc.wait() != 0:
        raise RuntimeError("Child exited with unexpected return code")

    if outp_needs_close:
        outp.close()

    if err_needs_close:
        err.close()

    if stin_needs_close:
        stin.close()

    return proc

class OmniplayEnvironment(object):
    def __init__(self, omniplay_location=None, pthread_lib=None, pin_root=None,
                    verbose=False):
        self.record_dir = "/replay_logdb"
        self.recording_suffix = "rec_"

        self.verbose = verbose

        # compute the home
        if omniplay_location:
            self.omniplay_location = omniplay_location
        else:
            if 'OMNIPLAY_DIR' not in os.environ:
                raise EnvironmentError("OMNIPLAY_DIR not defined, please run the setup script")

            self.omniplay_location = os.environ['OMNIPLAY_DIR']

        if pthread_lib:
            self.pthread = pthread_lib
            self.pthread_resume = pthread_lib
        else:
            self.pthread = ''.join([self.omniplay_location,
                "/eglibc-2.15/prefix/lib:/lib:/lib/i386-linux-gnu:/usr/lib:/usr/lib/i386-linux-gnu"])
            self.pthread_resume = ''.join([self.omniplay_location, "/eglibc-2.15/prefix/lib"])

        if pin_root:
            self.pin_root = pin_root
        else:
            home = os.path.expanduser("~")
            # default is $(HOME)/pin-2.13
            self.pin_root = '/'.join([home, "pin-2.13"])

        self.tools_location = '/'.join([self.omniplay_location, "pin_tools/obj-ia32"])

        self.logdb_dir = "/replay_logdb/"

        binaries = collections.namedtuple('Binary', ["record", "parseklog", "filemap", "replay", "parseckpt", "pin"])
        scripts = collections.namedtuple('Scripts', ["setup", "record", "insert", "run_pin"])

        self.scripts_dir = ''.join([self.omniplay_location, "/scripts"])
        self.scripts = scripts(
                setup = '/'.join([self.scripts_dir, "setup.sh"]),
                record = '/'.join([self.scripts_dir, "easy_launch.sh"]),
                insert = '/'.join([self.scripts_dir, "insert_spec.sh"]),
                run_pin = '/'.join([self.scripts_dir, "run_pin.sh"])
            )

        
        self.test_dir = ''.join([self.omniplay_location, "/test"])

        self.bins = binaries(
                replay = '/'.join([self.test_dir, "resume"]),
                record = '/'.join([self.test_dir, "launcher"]),
                parseklog = '/'.join([self.test_dir, "parseklog"]),
                filemap = '/'.join([self.test_dir, "filemap"]),
                parseckpt = '/'.join([self.test_dir, "parseckpt"]),
                pin = '/'.join([self.pin_root, "pin"])
            )

        self.setup_system()

    def setup_system(self):
        '''
        Check the system to see if all of the required binaries are there
        '''
        assert os.path.exists(self.omniplay_location)
        assert os.path.isdir(self.omniplay_location)

        assert os.path.exists(self.scripts_dir)
        assert os.path.isdir(self.scripts_dir)

        self.insert_spec()

        for pthread in self.pthread.split(':'):
            assert os.path.exists(pthread)
            assert os.path.isdir(pthread)
        assert os.path.exists(self.pin_root)
        assert os.path.isdir(self.tools_location)

        for binary in self.bins:
            assert os.path.exists(binary)

        for script in self.scripts:
            assert os.path.exists(script)

    def insert_spec(self):
        run_shell(self.scripts.insert)
        
    def record(self, cmd, stin=None, stout=None, sterr=None):
        # Get the binary
        args = shlex.split(cmd)
        args.reverse()
        command = args.pop()
        args.reverse()

        # Need to convert the binary into a full path
        whichcmd = ' '.join(["which", command])
        #print "whichcmd is " + whichcmd
        proc = subprocess.Popen(shlex.split(whichcmd), stdout=subprocess.PIPE)
        proc.wait()
        command = proc.stdout.read()
        # If which gives a relative command, append our cwd to it
        if command.startswith('./'):
            # -1 to trim \n
            command = '/'.join([os.getcwd(), command[2:]])

        while command[-1] == '\n':
            command = command[0:-1]

        #print "adjusted command to " + command

        # Get highest recorded process number
        if stout is None:
            stout = open(os.devnull, 'w')

        if sterr is None:
            sterr = open(os.devnull, 'w')

        if stin is None:
            stin = open(os.devnull, 'r')

        (fd, tmpname) = tempfile.mkstemp()
        os.close(fd)

        launcher = ' '.join([self.bins.record, "-o", tmpname, "-m", "--pthread", self.pthread])

        args = shlex.split(launcher) + [command] + args
        #print "about to run" + str(args)
        proc = subprocess.Popen(args, shell=False, stdin=stin, stdout=stout, stderr=sterr)
        proc.wait()
        #print "here?"
        
        with open(tmpname) as tmpfile:
            line = tmpfile.readlines()[0]
            match = re.match("Record log saved to: ([-0-9a-zA-Z._/]+)", line)
            logdir = match.group(1)

        os.remove(tmpname)

        ckptinfo = self.parseckpt(logdir)

        return ckptinfo

    def replay(self, replay_dir, pipe_log=None, pin=False, follow=False):
        '''
        @param replay_dir - replay directory
        @param pipe_log - open file handle
        '''
        cmd = ' '.join([self.bins.replay, "--pthread", self.pthread_resume, replay_dir])

        if pipe_log is None:
            pipe_log = open(os.devnull, 'w')

        if pin:
            cmd += " -p"
        if follow:
            cmd += " -f"

        if self.verbose:
            print(cmd)

        process = subprocess.Popen(shlex.split(cmd), shell=False, stdout=pipe_log, stderr=pipe_log)
        return process

    def attach_tool(self, pid, tool_name, pipe_output, flags=''):
        tool = tool_name
        # Check if tool_name exists...
        if not os.path.exists(tool):
            tool = '/'.join([self.tools_location, tool])

        assert(os.path.exists(tool))

        cmd = ' '.join([self.bins.pin, "-follow_execv", "-pid", str(pid), "-t", tool, flags])

        process = subprocess.Popen(shlex.split(cmd), shell=False, stdout=pipe_output)
        return process

    def run_tool(self, binary, tool, flags="", output="/tmp/stderr_log", toolout="/tmp/tool_log"):
        '''
        Uses replay and attach_tool to run the pin tool
        '''

        log_f = open(output, "w")
        tool_f = open(toolout, "w")

        replay_process = self.replay(binary, log_f, pin=True)

        # FIXME: Parse output for sleep info?
        time.sleep(1)

        attach_process = self.attach_tool(replay_process.pid, tool, tool_f, flags=flags)

        attach_process.wait()
        replay_process.wait()

    def filemap(self, filename, filemap_output="/tmp/filemap_output"):
        cmd = ' '.join([self.bins.filemap, filename, filemap_output])

        process = subprocess.Popen(shlex.split(cmd), shell=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return process

    def parseckpt(self, filename):
        cmd = ' '.join([self.bins.parseckpt, filename])

        proc = run_shell(cmd, outp=subprocess.PIPE)

        args = []
        env = {}

        with os.fdopen(os.dup(proc.stdout.fileno())) as f:
            for line in f:
                match = re.match("record pid: ([0-9]+)", line)
                if match is not None:
                    pid = int(match.group(1))
                    continue

                match = re.match("record group id: ([0-9]+)", line)
                if match is not None:
                    group_id = int(match.group(1))
                    continue

                match = re.match("parent record group id: ([0-9]+)", line)
                if match is not None:
                    parent_group_id = int(match.group(1))
                    continue

                match = re.match("record filename: (.+)", line)
                if match is not None:
                    exe = match.group(1)
                    continue

                match = re.match("Argument [0-9]+ is (.+)", line)
                if match is not None:
                    args.append(match.group(1))
                    continue

                match = re.match("Env\. var\. [0-9]+ is (.+)", line)
                if match is not None:
                    match = re.match("([-_a-zA-Z0-9]+)=(.*)", match.group(1))
                    env[match.group(1)] = match.group(2)
                    continue
                
                #print "Non matching line: " + line
                    
        return LogCkpt(
                pid = pid,
                group_id = group_id,
                parent_group_id = parent_group_id,
                exe = exe,
                args = args,
                env = env
            )

    def parseklog(self, klog, output):
        with open(output, 'w+') as f:
            print "Opened output " + output
            run_shell(' '.join([self.bins.parseklog, klog]), outp=f)

    def get_record_dir(self, record_group):
        subdir = ''.join([self.recording_suffix, str(record_group)])
        return '/'.join([self.record_dir, subdir])
