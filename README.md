# Limit Order Book / Matching Engine

Work in progress. A C++20 limit order book and matching engine, built as a
portfolio project for quant/HFT recruiting, with the optimisation work done
in public and benchmarked at every step rather than assumed.

The full README — headline latency numbers, the optimisation journey, and
the correctness methodology — gets written once that work exists to report
on. Until then, the commit history is the actual status report: it's
structured as reference engine and correctness harness first, real-data
validation and fuzzing second, benchmarking infrastructure third, and only
then a one-commit-per-technique optimisation arc.

## License

MIT
