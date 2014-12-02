# ``eaudit``: An Energy Auditing Tool

``eaudit`` provides a mechanism for associating program functions with the amount of energy they consume, analogous to the way gprof associates execution time with functions. This tool uses the RAPL energy counters to measure energy consumption. There are currently two versions:

1. ``instrumented``: This version inserts the measurement calls at the beginning and end of every function during compilation, ensuring that all energy is attributed appropriately. This incurs a higher runtime overhead.
2. ``tracing``: This version periodically samples the application, reducing the overhead of performance measurement but at the cost of lower accuracy in determining the precise functions to attribute energy.

## Building
Both projects are built by running ``make`` in the current directory. Please see the individual version directories for more details.
