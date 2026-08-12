import os
import sys
import time
import subprocess
import signal
from datetime import datetime

fstatus = open("job_status.log", "w")

# Ensure proper command-line arguments are provided
if len(sys.argv) < 3:
    print(f"Usage: python {sys.argv[0]} <path_to_job_file> <num_cpus>")
    sys.exit(1)

JOB_FILE = sys.argv[1]
NUM_CPUS = int(sys.argv[2])

# Read jobs line by line from the provided file
commands = []
if os.path.exists(JOB_FILE):
    with open(JOB_FILE, "r") as f:
        for line in f:
            cmd = line.strip()
            if cmd and not cmd.startswith("#"):
                commands.append(cmd)
else:
    fstatus.write(f"Error: Job file '{JOB_FILE}' not found.\n")
    fstatus.close()
    sys.exit(1)

if not commands:
    fstatus.write(f"Error: No valid jobs found in '{JOB_FILE}'.\n")
    fstatus.close()
    sys.exit(1)

# Set process group so Ctrl+C can terminate all spawned child jobs simultaneously
os.setpgrp()

def handle_sigint(sig, frame):
    fstatus.write("\n[!] Ctrl+C detected. Aborting all running jobs...\n")
    fstatus.flush()
    try:
        os.killpg(os.getpgrp(), signal.SIGKILL)
    except Exception as e:
        fstatus.write(f"Error killing process group: {e}\n")
    fstatus.close()
    sys.exit(1)

signal.signal(signal.SIGINT, handle_sigint)

human_start_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
fstatus.write(f"Starting execution of {len(commands)} jobs from '{JOB_FILE}' with concurrency limit: {NUM_CPUS}\n")
fstatus.write(f"Start Time: {human_start_time}\n\n")
fstatus.flush()

start_time = time.time()

active_processes = []
completed_jobs = []
cmd_iterator = iter(commands)

try:
    while True:
        # Fill up slots up to NUM_CPUS
        while len(active_processes) < NUM_CPUS:
            try:
                cmd = next(cmd_iterator)
                job_start = time.time()
                human_job_start = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                p = subprocess.Popen(cmd, shell=True, preexec_fn=os.setsid)
                active_processes.append({
                    "process": p,
                    "command": cmd,
                    "start_time": job_start
                })
                fstatus.write(f"[START] time: {human_job_start}: {cmd}\n")
                fstatus.flush()
            except StopIteration:
                break

        # Check status of running processes
        still_active = []
        for job in active_processes:
            p = job["process"]
            ret = p.poll()
            if ret is None:
                still_active.append(job)
            else:
                job_end = time.time()
                duration = job_end - job["start_time"]
                status = "DONE" if ret == 0 else "ABORT"
                completed_jobs.append((job["command"], status, ret, duration))
                fstatus.write(f"[{status}] (Exit: {ret}, Time: {duration:.2f}s): {job['command']}\n")
                fstatus.flush()

        active_processes = still_active

        # Break loop when all commands have been scheduled AND all active processes have finished
        if not active_processes:
            try:
                next(cmd_iterator)
            except StopIteration:
                break

        time.sleep(1)

except Exception as e:
    fstatus.write(f"\n[!] An error occurred during execution: {e}\n")
    fstatus.flush()
    try:
        os.killpg(os.getpgrp(), signal.SIGKILL)
    except Exception:
        pass
    sys.exit(1)
finally:
    end_time = time.time()
    total_duration = end_time - start_time
    human_end_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    fstatus.write("\n==============================\n")
    fstatus.write("All jobs finished or aborted!\n")
    fstatus.write(f"End Time: {human_end_time}\n")
    fstatus.write(f"Total Duration: {total_duration:.2f} seconds\n")
    fstatus.write("==============================\n")
    fstatus.close()