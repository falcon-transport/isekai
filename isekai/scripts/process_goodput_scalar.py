"""Process latency scalar results."""

import argparse

import pandas as pd


def filter_and_read_csv(file):
  """Read an ONET scalar file and extract average latency.

  Args:
    file: an OMET++ scalar file (*.sca).

  Returns:
    A pandas dataframe.
  """
  data = []
  with open(file, 'r') as file:
    for line in file:
      if 'goodput' in line:
        line = line.strip()
        data.append(line.split(' '))  # Assuming comma-separated values

  df = pd.DataFrame(
      data, columns=['type', 'src', 'dst', 'goodput']
  )  # Replace with your column names
  df['goodput'] = pd.to_numeric(df['goodput'])
  goodput = df.groupby('src')['goodput'].sum()
  return goodput


def main():
  parser = argparse.ArgumentParser(
      description='Process OMNET++ Vector files <input file> <result file>'
  )
  parser.add_argument(
      'input_file', type=str, help='Path to the input *.vec file'
  )
  parser.add_argument(
      'sim_duration',
      type=float,
      help='The simulation duration in seconds',
  )
  args = parser.parse_args()

  host_goodput = filter_and_read_csv(args.input_file)
  print(
      'Average goodput (Gbps)'
      f' {host_goodput.mean() / args.sim_duration}\n'
  )


if __name__ == '__main__':
  main()
