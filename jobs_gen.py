import os
import sys

# Ensure proper command-line arguments are provided
if len(sys.argv) < 4:
    print(f"Usage: python {sys.argv[0]} <path_to_config_file> <binary_path> <warmup_instructions> <simulation_instructions>")
    sys.exit(1)

CONFIG_FILE = sys.argv[1]
EXEC_PATH = sys.argv[2]
WARMUP = sys.argv[3]
SIM = sys.argv[4]

# Define the trace directory variable that matches the config format
TRACE_DIR = "/mnt/usb-Samsung_PSSD_T9_S743NS0X301609D-0:0-part1/pravesh"

# Extract the binary name from the path to use as the output directory (e.g., ./bin/foo -> foo)
BIN_NAME = os.path.basename(EXEC_PATH)

# Create the directory named after the binary if it doesn't exist
os.makedirs(BIN_NAME, exist_ok=True)

# Read and parse traces from the given config file
traces = []
if os.path.exists(CONFIG_FILE):
    with open(CONFIG_FILE, "r") as f:
        for line in f:
            if line.startswith("TRACE="):
                trace_path = line.strip().split("=", 1)[1]
                # Replace $(TRACE_DIR) with the actual expanded path variable
                trace_path = trace_path.replace("$(TRACE_DIR)", TRACE_DIR)
                traces.append(trace_path)
else:
    print(f"Error: Config file '{CONFIG_FILE}' not found.")
    sys.exit(1)

# Loop through and generate execution commands with output redirection
for trace_path in traces:
    # Extract filename from the expanded path (e.g., /mnt/usb-.../arizona_0002.champsim-042.gz -> arizona_0002.champsim-042.gz)
    filename = os.path.basename(trace_path)
    
    # Extract workload name (e.g., arizona_0002.champsim-042.gz -> arizona_0002)
    workload_name = filename.split(".champsim")[0]
    
    command = (
        f"{EXEC_PATH} -warmup_instructions {WARMUP} "
        f"-simulation_instructions {SIM} -traces {trace_path} "
        f"> {BIN_NAME}/{workload_name}.log 2>&1"
    )
    print(command)