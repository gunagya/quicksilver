#!/usr/bin/env python3
"""
Render a text instruction dump as a binned SVG qubit-load heatmap.

Each output row represents a contiguous instruction range. Each column is a qubit.
For each memory operation, the loaded qubit (operand 0) is colored according to
its source location immediately before the op:
  - memory       -> redder
  - intermediate -> greener

Locations are tracked through the trace using:
  - mswap(ld, st): swap locations(ld, st)
  - mplace(ld, st, evict): ld -> compute, st -> intermediate, evict -> memory

The initial residency is inferred from capacities:
  - qubits [0, c) start in compute
  - qubits [c, c + i) start in intermediate
  - remaining qubits start in memory

Capacities are parsed from the source binary filename when possible, or may be
provided explicitly on the command line. Row height scales with the instruction
bin size so coarser aggregations are easier to see.

This is designed for `qs_report -v` style output such as `build/inst.txt`,
including optional leading header lines like:
    [ QS_REPORT ] Number of qubits: 241
    [123] mswap 73 3

Usage example:
    python3 diagrams/render_instruction_bitmap.py build/inst.txt \
        --start 0 --end 100000 \
        --instruction-bin-size 100 \
        --output diagrams/ethylene_oxide_t_0_100000_bin100.svg
"""

from __future__ import annotations

import argparse
import html
import math
import re
from pathlib import Path


INSTRUCTION_LINE_RE = re.compile(r"^\[(\d+)\]\s+(.*)$")
NUM_QUBITS_RE = re.compile(r"^\[\s*QS_REPORT\s*\]\s*Number of qubits:\s*(\d+)\s*$")
SOURCE_FILE_RE = re.compile(r"^\[\s*QS_REPORT\s*\]\s*Reading binary file:\s*(.+?)\s*$")
INT_RE = re.compile(r"-?\d+")
COMPUTE_CAPACITY_RE = re.compile(r"_c(\d+)\b")
INTERMEDIATE_CAPACITY_RE = re.compile(r"_i(\d+)\b")

# Qubit arity by opcode. For instructions with angles or extra formatting,
# we take the last N integers on the line.
QUBIT_ARITY = {
    "nil": 0,
    "h": 1,
    "x": 1,
    "y": 1,
    "z": 1,
    "s": 1,
    "sx": 1,
    "sdg": 1,
    "sxdg": 1,
    "t": 1,
    "tx": 1,
    "tdg": 1,
    "txdg": 1,
    "rx": 1,
    "rz": 1,
    "mz": 1,
    "mx": 1,
    "cx": 2,
    "cz": 2,
    "swap": 2,
    "mswap": 2,
    "mprefetch": 2,
    "ccx": 3,
    "ccz": 3,
    "mplace": 3,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Render an instruction-range qubit bitmap as SVG.")
    parser.add_argument("input_file", type=Path, help="Instruction text file, e.g. build/inst.txt")
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Output SVG path. Defaults to diagrams/<input_stem>_<start>_<end>.svg",
    )
    parser.add_argument("--start", type=int, default=0, help="Inclusive starting instruction index")
    parser.add_argument(
        "--end",
        type=int,
        default=None,
        help="Exclusive ending instruction index. Defaults to the end of the file.",
    )
    parser.add_argument(
        "--instruction-bin-size",
        type=int,
        default=100,
        help="Number of instructions aggregated into each output row",
    )
    parser.add_argument(
        "--column-width",
        type=float,
        default=8.0,
        help="Width of a qubit column in SVG units",
    )
    parser.add_argument(
        "--row-height",
        type=float,
        default=0.2,
        help="Base height of a row in SVG units for bin size 100",
    )
    parser.add_argument(
        "--margin",
        type=float,
        default=24.0,
        help="Outer margin in SVG units",
    )
    parser.add_argument(
        "--label-height",
        type=float,
        default=24.0,
        help="Reserved space for the title block above the bitmap",
    )
    parser.add_argument(
        "--background",
        default="white",
        help="Background color",
    )
    parser.add_argument(
        "--compute-capacity",
        type=int,
        default=None,
        help="Override compute capacity c if it cannot be parsed from the filename",
    )
    parser.add_argument(
        "--intermediate-capacity",
        type=int,
        default=None,
        help="Override intermediate capacity i if it cannot be parsed from the filename",
    )
    return parser.parse_args()


def parse_instruction_line(line: str) -> tuple[int, str, list[int]] | None:
    match = INSTRUCTION_LINE_RE.match(line.strip())
    if not match:
        return None

    inst_idx = int(match.group(1))
    payload = match.group(2).strip()
    if not payload:
        return inst_idx, "", []

    opcode_match = re.match(r"^([a-z]+)", payload.lower())
    if not opcode_match:
        raise ValueError(f"Could not parse opcode from instruction line: {line.rstrip()}")
    opcode = opcode_match.group(1)
    qubit_arity = QUBIT_ARITY.get(opcode)
    if qubit_arity is None:
        raise ValueError(f"Unknown opcode in instruction dump: {opcode!r}")

    if qubit_arity == 0:
        return inst_idx, opcode, []

    ints = [int(x) for x in INT_RE.findall(payload)]
    if len(ints) < qubit_arity:
        raise ValueError(
            f"Could not extract {qubit_arity} qubit indices from instruction line: {line.rstrip()}"
        )

    return inst_idx, opcode, ints[-qubit_arity:]


def load_instruction_rows(
    input_file: Path, start: int, end: int | None
) -> tuple[int | None, str | None, list[tuple[int, str, list[int]]]]:
    num_qubits = None
    source_file = None
    rows: list[tuple[int, str, list[int]]] = []

    with input_file.open("r", encoding="utf-8") as f:
        for raw_line in f:
            line = raw_line.rstrip("\n")
            nq_match = NUM_QUBITS_RE.match(line)
            if nq_match:
                num_qubits = int(nq_match.group(1))
                continue
            src_match = SOURCE_FILE_RE.match(line)
            if src_match:
                source_file = src_match.group(1)
                continue

            parsed = parse_instruction_line(line)
            if parsed is None:
                continue

            inst_idx, opcode, qubits = parsed
            if inst_idx < start:
                continue
            if end is not None and inst_idx >= end:
                continue
            if opcode not in {"mswap", "mplace"}:
                continue

            rows.append((inst_idx, opcode, qubits))

    return num_qubits, source_file, rows


def infer_num_qubits(header_num_qubits: int | None, rows: list[tuple[int, str, list[int]]]) -> int:
    if header_num_qubits is not None:
        return header_num_qubits
    max_qubit = max((q for _, _, qubits in rows for q in qubits), default=-1)
    return max_qubit + 1


def parse_capacities_from_name(name: str) -> tuple[int | None, int | None]:
    c_match = COMPUTE_CAPACITY_RE.search(name)
    i_match = INTERMEDIATE_CAPACITY_RE.search(name)
    c = int(c_match.group(1)) if c_match else None
    i = int(i_match.group(1)) if i_match else None
    return c, i


def resolve_capacities(
    input_file: Path,
    source_file: str | None,
    compute_capacity_override: int | None,
    intermediate_capacity_override: int | None,
) -> tuple[int, int]:
    compute_capacity = compute_capacity_override
    intermediate_capacity = intermediate_capacity_override

    candidate_names = []
    if source_file:
        candidate_names.append(source_file)
        candidate_names.append(Path(source_file).name)
    candidate_names.append(str(input_file))
    candidate_names.append(input_file.name)

    for candidate in candidate_names:
        parsed_c, parsed_i = parse_capacities_from_name(candidate)
        if compute_capacity is None and parsed_c is not None:
            compute_capacity = parsed_c
        if intermediate_capacity is None and parsed_i is not None:
            intermediate_capacity = parsed_i

    if compute_capacity is None or intermediate_capacity is None:
        raise SystemExit(
            "Could not infer both compute capacity and intermediate capacity from the filename. "
            "Pass --compute-capacity and --intermediate-capacity explicitly."
        )

    return compute_capacity, intermediate_capacity


def aggregate_instruction_bins(
    rows: list[tuple[int, str, list[int]]],
    start: int,
    end: int,
    bin_size: int,
    num_qubits: int,
    compute_capacity: int,
    intermediate_capacity: int,
) -> tuple[list[list[int]], list[list[int]], int, int]:
    row_count = max(1, math.ceil((end - start) / bin_size))
    memory_bins = [[0 for _ in range(num_qubits)] for _ in range(row_count)]
    intermediate_bins = [[0 for _ in range(num_qubits)] for _ in range(row_count)]
    total_memory_loads = 0
    total_intermediate_loads = 0

    if compute_capacity < 0 or intermediate_capacity < 0:
        raise SystemExit("Capacities must be non-negative")
    if compute_capacity + intermediate_capacity > num_qubits:
        raise SystemExit(
            f"Invalid capacities: compute={compute_capacity}, intermediate={intermediate_capacity}, "
            f"num_qubits={num_qubits}"
        )

    locations = ["memory" for _ in range(num_qubits)]
    for qubit in range(min(compute_capacity, num_qubits)):
        locations[qubit] = "compute"
    for qubit in range(compute_capacity, min(compute_capacity + intermediate_capacity, num_qubits)):
        locations[qubit] = "intermediate"

    for inst_idx, opcode, qubits in rows:
        bin_idx = (inst_idx - start) // bin_size
        if not (0 <= bin_idx < row_count):
            continue

        ld = qubits[0]
        ld_source = locations[ld]
        if ld_source == "memory":
            memory_bins[bin_idx][ld] += 1
            total_memory_loads += 1
        elif ld_source == "intermediate":
            intermediate_bins[bin_idx][ld] += 1
            total_intermediate_loads += 1

        if opcode == "mswap":
            st = qubits[1]
            locations[ld], locations[st] = locations[st], locations[ld]
        elif opcode == "mplace":
            st, evict = qubits[1], qubits[2]
            locations[ld] = "compute"
            locations[st] = "intermediate"
            locations[evict] = "memory"

    return memory_bins, intermediate_bins, total_memory_loads, total_intermediate_loads


def usage_to_color(memory_count: int, intermediate_count: int, max_count: int) -> str:
    if (memory_count + intermediate_count) <= 0 or max_count <= 0:
        return "rgb(255,255,255)"

    red_strength = int(round(220 * (memory_count / max_count)))
    green_strength = int(round(220 * (intermediate_count / max_count)))
    red_strength = max(0, min(220, red_strength))
    green_strength = max(0, min(220, green_strength))

    r = 255 - green_strength
    g = 255 - red_strength
    b = 255 - max(red_strength, green_strength)
    return f"rgb({r},{g},{b})"


def scaled_row_height(base_row_height: float, instruction_bin_size: int) -> float:
    return base_row_height * (instruction_bin_size / 100.0)


def render_svg(
    output_file: Path,
    input_file: Path,
    memory_bins: list[list[int]],
    intermediate_bins: list[list[int]],
    num_qubits: int,
    start: int,
    end: int | None,
    instruction_bin_size: int,
    compute_capacity: int,
    intermediate_capacity: int,
    column_width: float,
    row_height: float,
    margin: float,
    label_height: float,
    background: str,
    hit_rate_percentage: float,
) -> None:
    font_size = 33
    small_font_size = 30
    row_count = len(memory_bins)
    left_axis_width = max(140.0, font_size * 3.8)
    bottom_axis_height = max(90.0, font_size * 2.6)
    legend_width = max(310.0, font_size * 8.5)
    bitmap_width = num_qubits * column_width
    bitmap_height = row_count * row_height
    width = 2 * margin + left_axis_width + bitmap_width + legend_width
    height = 2 * margin + bitmap_height + bottom_axis_height
    max_usage = max(
        (
            memory_bins[row_idx][qubit] + intermediate_bins[row_idx][qubit]
            for row_idx in range(row_count)
            for qubit in range(num_qubits)
        ),
        default=0,
    )

    x0 = margin + left_axis_width
    y0 = margin

    with output_file.open("w", encoding="utf-8") as out:
        out.write('<?xml version="1.0" encoding="UTF-8"?>\n')
        out.write(
            f'<svg xmlns="http://www.w3.org/2000/svg" version="1.1" '
            f'width="{width}" height="{height}" viewBox="0 0 {width} {height}">\n'
        )
        out.write(
            f'  <rect x="0" y="0" width="{width}" height="{height}" fill="{html.escape(background)}"/>\n'
        )
        out.write(
            f"  <title>{html.escape(f'Instruction load map: qubits={num_qubits}, c={compute_capacity}, i={intermediate_capacity}, bin_size={instruction_bin_size}, max_loads_per_cell={max_usage}')}</title>\n"
        )
        out.write('  <g shape-rendering="crispEdges">\n')
        out.write(
            f'    <rect x="{x0}" y="{y0}" width="{bitmap_width}" height="{bitmap_height}" '
            'fill="none" stroke="#cccccc" stroke-width="0.5"/>\n'
        )

        for row_offset in range(row_count):
            y = y0 + row_offset * row_height
            for qubit in range(num_qubits):
                memory_count = memory_bins[row_offset][qubit]
                intermediate_count = intermediate_bins[row_offset][qubit]
                if memory_count == 0 and intermediate_count == 0:
                    continue
                x = x0 + qubit * column_width
                out.write(
                    f'    <rect x="{x}" y="{y}" width="{column_width}" height="{row_height}" '
                    f'fill="{usage_to_color(memory_count, intermediate_count, max_usage)}"/>\n'
                )

        out.write("  </g>\n")
        out.write(f'  <g font-family="monospace" font-size="{small_font_size}" fill="black">\n')
        out.write(
            f'    <line x1="{x0}" y1="{y0 + bitmap_height}" x2="{x0 + bitmap_width}" y2="{y0 + bitmap_height}" stroke="black" stroke-width="1"/>\n'
        )
        out.write(
            f'    <line x1="{x0}" y1="{y0}" x2="{x0}" y2="{y0 + bitmap_height}" stroke="black" stroke-width="1"/>\n'
        )

        x_tick_count = min(6, max(2, num_qubits))
        for tick_idx in range(x_tick_count):
            qubit_value = 0 if x_tick_count == 1 else round((num_qubits - 1) * tick_idx / (x_tick_count - 1))
            tick_x = x0 + qubit_value * column_width
            out.write(
                f'    <line x1="{tick_x}" y1="{y0 + bitmap_height}" x2="{tick_x}" y2="{y0 + bitmap_height + 5}" stroke="black" stroke-width="1"/>\n'
            )
            out.write(
                f'    <text x="{tick_x}" y="{y0 + bitmap_height + small_font_size + 8}" text-anchor="middle">{qubit_value}</text>\n'
            )

        y_tick_count = min(6, max(2, row_count))
        for tick_idx in range(y_tick_count):
            bin_value = 0 if y_tick_count == 1 else round((row_count - 1) * tick_idx / (y_tick_count - 1))
            inst_value = start + bin_value * instruction_bin_size
            tick_y = y0 + bin_value * row_height
            out.write(
                f'    <line x1="{x0 - 5}" y1="{tick_y}" x2="{x0}" y2="{tick_y}" stroke="black" stroke-width="1"/>\n'
            )
            out.write(
                f'    <text x="{x0 - 12}" y="{tick_y + small_font_size * 0.35}" text-anchor="end">{inst_value}</text>\n'
            )

        out.write(
            f'    <text x="{x0 + bitmap_width / 2}" y="{y0 + bitmap_height + bottom_axis_height - 16}" text-anchor="middle" font-size="{font_size}">qubits</text>\n'
        )
        out.write(
            f'    <text x="{margin + font_size * 0.9}" y="{y0 + bitmap_height / 2}" text-anchor="middle" font-size="{font_size}" transform="rotate(-90 {margin + font_size * 0.9} {y0 + bitmap_height / 2})">instructions (bin size = {instruction_bin_size})</text>\n'
        )

        legend_x = x0 + bitmap_width + 18
        legend_y = y0 + font_size + 10
        legend_box = max(20, small_font_size * 0.7)
        legend_gap = max(22, small_font_size + 8)
        out.write(f'    <text x="{legend_x}" y="{legend_y - 12}" font-size="{font_size}">legend</text>\n')
        out.write(
            f'    <rect x="{legend_x}" y="{legend_y}" width="{legend_box}" height="{legend_box}" fill="rgb(255,35,35)" stroke="#666666" stroke-width="0.5"/>\n'
        )
        out.write(
            f'    <text x="{legend_x + legend_box + 14}" y="{legend_y + legend_box - 2}">memory load</text>\n'
        )
        out.write(
            f'    <rect x="{legend_x}" y="{legend_y + legend_gap}" width="{legend_box}" height="{legend_box}" fill="rgb(35,255,35)" stroke="#666666" stroke-width="0.5"/>\n'
        )
        out.write(
            f'    <text x="{legend_x + legend_box + 14}" y="{legend_y + legend_gap + legend_box - 2}">intermediate load</text>\n'
        )
        out.write(
            f'    <text x="{legend_x}" y="{legend_y + legend_gap + legend_box + font_size + 8}" font-size="{font_size}">hit rate %: {hit_rate_percentage:.2f}</text>\n'
        )
        out.write("  </g>\n")
        out.write("</svg>\n")


def main() -> None:
    args = parse_args()

    if args.start < 0:
        raise SystemExit("--start must be >= 0")
    if args.end is not None and args.end <= args.start:
        raise SystemExit("--end must be greater than --start")
    if args.instruction_bin_size <= 0:
        raise SystemExit("--instruction-bin-size must be positive")
    if args.column_width <= 0 or args.row_height <= 0:
        raise SystemExit("--column-width and --row-height must be positive")

    header_num_qubits, source_file, rows = load_instruction_rows(
        args.input_file, args.start, args.end
    )
    if not rows:
        raise SystemExit("No MSWAP/MPLACE instructions found in the requested range")

    num_qubits = infer_num_qubits(header_num_qubits, rows)
    compute_capacity, intermediate_capacity = resolve_capacities(
        args.input_file,
        source_file,
        args.compute_capacity,
        args.intermediate_capacity,
    )
    actual_end = args.end if args.end is not None else rows[-1][0] + 1
    memory_bins, intermediate_bins, total_memory_loads, total_intermediate_loads = aggregate_instruction_bins(
        rows,
        start=args.start,
        end=actual_end,
        bin_size=args.instruction_bin_size,
        num_qubits=num_qubits,
        compute_capacity=compute_capacity,
        intermediate_capacity=intermediate_capacity,
    )
    effective_row_height = scaled_row_height(args.row_height, args.instruction_bin_size)
    total_loads = total_memory_loads + total_intermediate_loads
    hit_rate_percentage = (
        100.0 * total_intermediate_loads / total_loads
        if total_loads > 0 else 0.0
    )

    default_output = Path("diagrams") / (
        f"{args.input_file.stem}_{args.start}_{'end' if args.end is None else args.end}"
        f"_bin{args.instruction_bin_size}.svg"
    )
    output_file = args.output or default_output
    output_file.parent.mkdir(parents=True, exist_ok=True)

    render_svg(
        output_file=output_file,
        input_file=args.input_file,
        memory_bins=memory_bins,
        intermediate_bins=intermediate_bins,
        num_qubits=num_qubits,
        start=args.start,
        end=args.end,
        instruction_bin_size=args.instruction_bin_size,
        compute_capacity=compute_capacity,
        intermediate_capacity=intermediate_capacity,
        column_width=args.column_width,
        row_height=effective_row_height,
        margin=args.margin,
        label_height=args.label_height,
        background=args.background,
        hit_rate_percentage=hit_rate_percentage,
    )

    print(f"Wrote {output_file}")
    print(f"Binned rows: {len(memory_bins)}")
    print(f"Qubits: {num_qubits}")
    print(f"Compute capacity: {compute_capacity}")
    print(f"Intermediate capacity: {intermediate_capacity}")
    print(f"Effective row height: {effective_row_height}")
    print(f"Intermediate-load hit rate %: {hit_rate_percentage:.2f}")


if __name__ == "__main__":
    main()
