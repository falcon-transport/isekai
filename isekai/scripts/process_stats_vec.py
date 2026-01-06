"""Process latency vector results."""

import argparse

import pandas as pd


def vector_file_to_df(file, metric_name):
  """Analyze a vector file and extract min/mean/median/p99/p99.9/max.

  Args:
    file: an OMET++ vector file (*.vec).
    metric_name: the name of vector metric to get percentiles.

  Returns:
    A pandas dataframe.
  """
  with open(file, 'r') as file:
    vector_ids = []
    data = []

    for line in file:
      line = line.strip().split()

      if not line:
        continue

      if line[0] == 'vector' and metric_name in line[3]:
        vector_ids.append(line[1])
        continue

      if len(line) == 4 and line[0] in vector_ids:
        data += [[float(line[3])]]

    df = pd.DataFrame(data, columns=['value'])

  return df


def process_stats(df):
  stats_min = df['value'].min()
  stats_mean = df['value'].mean()
  stats_p50 = df['value'].median()
  stats_p99 = df['value'].quantile(0.99)
  stats_p999 = df['value'].quantile(0.999)
  stats_max = df['value'].max()
  return (stats_min, stats_mean, stats_p50, stats_p99, stats_p999, stats_max)


def main():
  parser = argparse.ArgumentParser(
      description=(
          'Parse statistics of a vector metric <metric_name> from OMNET++'
          ' Vector file <input file>'
      )
  )
  parser.add_argument(
      'input_file', type=str, help='Path to the input *.vec file'
  )
  parser.add_argument(
      'metric_name', type=str, help='Vector metric name to get percentiles'
  )
  args = parser.parse_args()
  df = vector_file_to_df(args.input_file, args.metric_name)
  stats_min, stats_mean, stats_p50, stats_p99, stats_p999, stats_max = (
      process_stats(df)
  )
  print(f'min: {stats_min}\n')
  print(f'mean: {stats_mean}\n')
  print(f'median: {stats_p50}\n')
  print(f'p99: {stats_p99}\n')
  print(f'p99.9: {stats_p999}\n')
  print(f'max: {stats_max}\n')


if __name__ == '__main__':
  main()
