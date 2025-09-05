"""Analyze a vector file and extract average/p90/p99 latency.

Args:
  file: an OMET++ scalar file (*.vec).

Returns:
  A pandas dataframe.
"""

import argparse

from omnetpp.scave import results


def vector_file_to_df(file):
  """Read an ONET vector file and extract average/p90/p99 latency.

  Args:
    file: an OMET++ scalar file (*.vec).

  Returns:
    A pandas dataframe.
  """
  # use OMNET++ APIs to read a *.vec file into a pandas dataframe
  raw_df = results.read_result_files(
      file, 'module =~ "**.host*" AND name =~ "**.op_latency*"'
  )
  # the pandas frame contains everything int the *vec file, use OMNET++ APIs
  # to extract only the vectors
  scalars = results.get_vectors(raw_df)
  # Filter only the vectors of intrest. For this use case only "latency"
  # vectors
  filtered_df = scalars[scalars['name'].str.contains('latency')]
  # Cleanup columns that don't have relevant information to speed up the
  # processing of the pandas dataframe
  numeric_df = filtered_df.drop(columns=['runID', 'module', 'name', 'vectime'])

  # OMNET++ stores a numpy vector as values. "explode" flattens the vector
  # for easier processing of pandas stats
  df = numeric_df.explode('vecvalue')
  return df


def process_stats(df):
  mean = df['vecvalue'].mean()
  p50 = df['vecvalue'].median()
  p99 = df['vecvalue'].quantile(0.99)
  return (mean, p50, p99)


def main():
  parser = argparse.ArgumentParser(
      description='Process OMNET++ Vector file <input file><result file>'
  )
  parser.add_argument(
      'input_file', type=str, help='Path to the input *.vec file'
  )
  args = parser.parse_args()
  df = vector_file_to_df(args.input_file)
  mean, p50, p99 = process_stats(df)
  print(f'mean_latency: {mean}\n')
  print(f'median_latency: {p50}\n')
  print(f'p99_latency: {p99}\n')


if __name__ == '__main__':
  main()
