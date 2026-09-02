import os
import sys
import re
import glob
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

# 1. Define the regex pattern to extract parameters from the file name
# Example: rtt_rmw_tickle_i100_s1024_c10000.csv 
# extracts: middleware='rabbitmq', interval_ms=100, payload_size=1024, message_count=10000
# Python named capturing group syntax: (?P<group_name>pattern)
file_pattern = re.compile(
    r"rtt_(?P<middleware>(?:rmw_)?[^_]+(?:_cpp)?)_i(?P<interval_ms>\d+)_s(?P<payload_size>\d+)_c(?P<message_count>\d+)\.csv"
)

parsed_data = []

argc = len(sys.argv)
path_prefix = ""
if argc == 1:
    print("CSV files will be searched in current working directory")
elif argc == 2:
    print(f"CSV files will be searched in {sys.argv[1]}")
    path_prefix = sys.argv[1]
else:
    print("too many arguments")
    quit()

os.chdir(path_prefix)

# 2. Find and process all matching CSV files in the current working directory
for file_path in glob.glob("rtt_*.csv"):
    filename = os.path.basename(file_path)
    match = file_pattern.match(filename)
    
    if match:
        meta = match.groupdict()
        try:
            # Load CSV file. 
            # Note: If your CSV files do NOT have a header row, keep header=None and names as defined.
            # If your CSV files DO have a header row, change to header=0 and use the exact column name for RTT.
            df = pd.read_csv(file_path, names=['count', 'timestamp_ns', 'rtt_ns'], header=0)
            
            # Compute the average RTT for this specific file
            avg_rtt_ms = df['rtt_ns'].mean() / 1000000
            
            # Append parsed metadata and the aggregated result
            parsed_data.append({
                'middleware': meta['middleware'],
                'interval_ms': int(meta['interval_ms']),
                'payload_size': int(meta['payload_size']),
                'message_count': int(meta['message_count']),
                'avg_rtt_ms': avg_rtt_ms
            })
        except Exception as e:
            print(f"Skipping {filename} due to error: {e}")

# 3. Create a unified DataFrame from the gathered results
df_results = pd.DataFrame(parsed_data)

if df_results.empty:
    print("No data extracted. Verify that your CSV filenames match the pattern and reside in this directory.")
    quit()

# Sort by interval_ms so that the line graph connects points sequentially along the x-axis
df_results = df_results.sort_values(by='interval_ms')
df_intervals = df_results['interval_ms'].drop_duplicates()

# Group by payload_size and message_count to generate a separate plot for each unique testing environment
grouped_configs = df_results.groupby(['payload_size', 'message_count'])

for (payload, count), config_group in grouped_configs:
    # Clear the current plotting canvas for a clean figure
    plt.clf()
    
    # Plot a separate line for each middleware within this payload/count combination
    for middleware, middleware_group in config_group.groupby('middleware'):
        intervals = np.arange(df_intervals.size)
        plt.plot(
            intervals, 
            middleware_group['avg_rtt_ms'], 
            marker='o', 
            linestyle='-', 
            linewidth=2,
            label=middleware,
        )
        plt.xticks(intervals, df_intervals)
    
    # Add labels, titles, and legend configurations
    plt.xlabel("Time Interval (ms)")
    plt.ylabel("Average RTT (ms)")
    plt.ylim(0, 1)
    plt.title(f"Average RTT vs Time Interval\n(Payload Size: {payload} bytes, Message Count: {count})")
    plt.grid(True, linestyle='--', alpha=0.5)
    plt.legend(title="Middleware")
    plt.tight_layout()
    
    # Save the figure to a file
    output_image = f"rtt_vs_interval_s{payload}_c{count}.png"
    plt.savefig(output_image, dpi=300)
    print(f"Successfully generated and saved plot: {output_image}")
