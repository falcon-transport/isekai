"""Process scalar results and extract metric."""

import argparse

import pandas as pd


def filter_and_read_csv(file, metric_name, group_by, reducer, is_rate):
  """Read an ONET scalar file and extract a scalar metic.

  Args:
    file: an OMET++ scalar file (*.sca).
    metric_name: a scalar metric to parse (e.g., goodput).
    group_by: aggregation unit to group (src,dst/src/dst) with sum.
    reducer: reducer for metrics (sum/mean/min/median/max/var).
    is_rate: whether the metric is rate or not. If true, the final metric value
      is divided by total simulation time to comput rate.

  Returns:
    A pandas dataframe.
  """
  metric_data = []
  simulation_time_sec = 0.0
  with open(file, 'r') as file:
    for line in file:
      if metric_name in line:
        line = line.strip()
        metric_data.append(line.split(' '))  # Assuming space-separated values
      if 'elapsed_time_us' in line:
        simulation_time_sec = float(line.strip().split(' ')[-1]) / 1e6

  if not metric_data:
    print(f'[ERROR] Cannot find metric: {metric_name}.')
    return 0

  df = pd.DataFrame(metric_data, columns=['type', 'src', 'dst', 'value'])
  df['value'] = pd.to_numeric(df['value'])

  # Group metrics
  if group_by not in ['src,dst', 'src', 'dst']:
    print(
        f'[WARNING] cannot group by {group_by}. Using default group by src,dst.'
    )
    group_by = 'src,dst'
  grouped_metric = df.groupby(group_by.split(','))['value'].sum()

  # Reducing metrics
  if reducer == 'sum':
    metric = grouped_metric.sum()
  elif reducer == 'mean':
    metric = grouped_metric.mean()
  elif reducer == 'min':
    metric = grouped_metric.min()
  elif reducer == 'median':
    metric = grouped_metric.median()
  elif reducer == 'max':
    metric = grouped_metric.max()
  elif reducer == 'var':
    metric = grouped_metric.var()
  else:
    print(f'Unsupported reducer: {reducer}. Using mean by default.')
    metric = grouped_metric.mean()

  # If metric is rate, divide it by total simulation time
  if is_rate:
    metric = metric / simulation_time_sec

  # handle double couting due to 'goodput' / 'goodput.src.dst.pattern'
  if metric_name == 'goodput':
    metric = metric / 2

  return metric


def main():
  parser = argparse.ArgumentParser(
      description='Process OMNET++ scalar file <input file>'
  )
  parser.add_argument(
      'input_file', type=str, help='Path to the input *.sca file'
  )
  parser.add_argument(
      'metric_name', type=str, help='Scalar metric name to parse'
  )
  parser.add_argument(
      '--group_by',
      type=str,
      default='src,dst',
      help='Aggregation unit to group (src/dst/src,dst) with sum',
  )
  parser.add_argument(
      '--reducer',
      type=str,
      default='mean',
      help='Metric reducer (sum/mean/min/median/max/var)',
  )
  parser.add_argument(
      '--is_rate',
      action='store_true',
      help=(
          'If set, the metric is divided by the total simulation time to'
          ' compute rate'
      ),
  )

  args = parser.parse_args()

  value = filter_and_read_csv(
      args.input_file,
      args.metric_name,
      args.group_by,
      args.reducer,
      args.is_rate,
  )
  print(
      f'{args.reducer} {args.metric_name}{"/s" if args.is_rate else ""} (per'
      f' {args.group_by}): {value}\n'
  )

if __name__ == '__main__':
  main()
