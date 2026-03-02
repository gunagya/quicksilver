Make a modification to get_memory_instructions which returns an order along with the instruction. The order is the layer number if fetching by layers and depth if fetching by circuit depth.

Make yoked_1d_storage map of needed qubits store the (order, qubit) so that qubits with a lower value of order (depth or layer) are fetched first from 2D storage.