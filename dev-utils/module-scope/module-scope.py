#
# This will be a very simple python script for visualizing data coming from the serial port of an esp32
# Any line starting with [DATA] will contain state data that should be 
#
# The format will contain flattened '{json state data restricted to int, float, and bool types}'
# i.e. [DATA] '{"rising edge":true, "falling edge":false, "acceleration":1.25}'
#
# If a [DATA] flagged line contains incompatible data, display a small warning 
#   "WARN: data contains ignored data types"
#
# 