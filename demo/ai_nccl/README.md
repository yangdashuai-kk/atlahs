# NCCL Demo

Minimal ATLAHS LLAMA/NCCL demo for the 2-node LLAMA 7B case.

Only the traced `sbatch` step is cached. The login-node stages are meant to be shown live and regenerate their outputs under `runs/llama7b_n2/`.

1. `./01_submit_megatron.sh`
   Prints or validates the 2-node `sbatch` command. Real runs use `srun --environment=megatron` on `debug`.
2. `./02_export_sqlite.sh`
3. `./03_generate_goal_v2.sh`
4. `./04_run_loggopsim.sh`
5. `./05_run_htsim.sh`
6. `./06_show_summary.sh`

For the live demo, `cache/trace_job/raw_nsys/` is pre-seeded from the local LLAMA evaluation run and stands in for the already-completed traced job. SQLite export, V2 GOAL generation, LogGOPSim, and HTSIM are rerun locally into `runs/llama7b_n2/` each time.

The demo uses LGS with `-L 3700 -o 200 -g 5 -O 0 -G 0.04 -S 0` and HTSIM with a demo-local 8-node, 200 Gbps leaf-spine topology under `topologies/leaf_spine_8_1os.topo`.

## Replay a GOAL trace on a server adjacency matrix

`07_run_htsim_matrix.sh` runs the local Llama GOAL binary on a server-level adjacency matrix instead of the leaf-spine topology. Its small default example has two servers with one directed lane in each direction:

```bash
./07_run_htsim_matrix.sh
```

The default trace is `workloads/llama7b_2n8g/llama2_7b_2n8g.bin`. You can substitute your generated matrix and global route table:

```bash
HTSIM_MATRIX=/path/to/topology.csv \
HTSIM_ROUTES=/path/to/routes.json \
HTSIM_LINKSPEED=400000 \
./07_run_htsim_matrix.sh
```

For an input-only check, append `--check-only`. This reads only the GOAL binary
header and the small configuration files; it does not start Htsim or build code.
The downloaded `llama2_7b_2n8g.bin` has **2 GOAL rank schedules**, 45 CPU
resource IDs per rank and 4 logical NIC IDs per rank. Eight GPUs do not imply
eight GOAL schedules in this encoding. With the two-server matrix its GOAL
rank-to-server map is `[0,1]`, rather than an eight-entry GPU placement map.
For example, `[0,2,2,0]` represents two directed lanes in each direction.

`-matrix` accepts the `topology.json` emitted by the repository's `generator.py`, the matrix CSV format (`src/dst,0,1,...` followed by indexed rows), or a flat list of `N*N` integers. For JSON, the backend reads `config.num_servers` and the row-major `global` matrix. Each matrix value is a count of parallel directed links; `-linkspeed` is the bandwidth of one lane. The route file accepts the generated JSON format (`src`, `dst`, `reachable`, `paths`) or a CSV with `src,dst,path`. If `HTSIM_ROUTES` is empty, Htsim derives deterministic shortest paths from the matrix.

The GOAL rank count must divide evenly across servers for the default placement, which maps contiguous rank ranges to server IDs. For a different placement, pass `HTSIM_RANK_SERVER_MAP=/path/to/rank_server_map.txt` with one server ID per GOAL rank, one per line. Inter-server data and reverse ACK traffic follow the selected global routes; multiple route alternatives are selected once per flow, and parallel lanes are assigned by flow ID. Inter-server flows from ranks on the same server share its aggregate ingress and egress queues. Explicit same-server sends use a separate shared local fabric, configured by JSON `config.local_speed_mbps` (default 1200000) and `config.local_latency_ns` (default 0). These defaults are modeling assumptions, not hardware information extracted from the GOAL file. LGS's fixed extra 200-Gbps injection gate is disabled in matrix mode so Htsim queues enforce bandwidth. The matrix CLI is currently wired into `htsim_uec`; linking the adapter into other executables does not enable their matrix CLI.

These source changes require rebuilding. Input checks do not validate the C++
build, transport behavior, full-trace correctness, or peak memory consumption.
Replay defaults to a 1536-MiB virtual-address-space limit and disables core dumps.
Set `HTSIM_MEMORY_MB` to a different MiB limit on the server (`0` disables it).
