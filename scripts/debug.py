import os
import sys

DEBUGFS_MOUNT_POINT = "/sys/kernel/debug"
TARGET_PID_PATH = os.path.join(DEBUGFS_MOUNT_POINT, "ss_dbg", "target_pid")
PGD_PATH = os.path.join(DEBUGFS_MOUNT_POINT, "ss_dbg", "pgd_hi")

# 4-level paging: each PGD entry maps 2^39 bytes = 0.5 TiB
PGDIR_SHIFT = 39
TIB_PER_PGD_ENTRY = (1 << PGDIR_SHIFT) / (1 << 40)

def pgd_index_to_addr(idx):
    """Canonical virtual address of the first byte mapped by a PGD entry."""
    addr = idx << PGDIR_SHIFT
    if addr & (1 << 47):
        addr |= 0xffff000000000000
    return addr


def get_all_pids():
    for dirname in os.listdir('/proc'):
        if dirname.isdigit():
            yield int(dirname)

def main():
    if os.geteuid() != 0:
        print("this script must be run as root to access /sys/kernel/debug/")
        sys.exit(1)

    if not os.path.exists(TARGET_PID_PATH) or not os.path.exists(PGD_PATH):
        print("debugfs files not found")
        sys.exit(1)

    pids = list(get_all_pids())
    reference_pid = None
    reference_indices = None
    divergent_pids = {}  # pid -> dict of differences
    successful_scans = 0

    for pid in pids:
        try:
            with open(TARGET_PID_PATH, 'w') as f:
                f.write(str(pid))
            
            current_indices = set()
            
            with open(PGD_PATH, 'r') as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith('#') or line.startswith('index') or line.startswith('error:'):
                        continue
                    parts = line.split(',')
                    if len(parts) == 2:
                        idx = int(parts[0].strip())
                        if idx >= 256:
                            current_indices.add(idx)

            if not current_indices:
                continue

            successful_scans += 1

            if reference_indices is None:
                reference_indices = current_indices
                reference_pid = pid
            else:
                if current_indices != reference_indices:
                    missing = reference_indices - current_indices
                    extra = current_indices - reference_indices
                    divergent_pids[pid] = {
                        "missing": sorted(list(missing)),
                        "extra": sorted(list(extra))
                    }

        except ProcessLookupError:
            pass
        except OSError:
            pass

    if reference_indices is None:
        print("could not read valid upper-PGD indices for any processes")
        sys.exit(1)

    print(f"total non-empty kernel indices (>= 256): {len(reference_indices)}")
    
    sorted_baseline = sorted(list(reference_indices))
    for i in range(0, len(sorted_baseline), 10):
        chunk = sorted_baseline[i:i+10]
        print(", ".join(f"{idx:3d}" for idx in chunk))

    print("\ncontiguous empty PGD Indices:")
    max_idx = max(511, sorted_baseline[-1]) if sorted_baseline else 511
    
    empty_ranges = []
    start_empty = None
    
    for i in range(256, max_idx + 1):
        if i not in reference_indices:
            if start_empty is None:
                start_empty = i
        else:
            if start_empty is not None:
                empty_ranges.append((start_empty, i - 1))
                start_empty = None
                
    if start_empty is not None:
        empty_ranges.append((start_empty, max_idx))

    if not empty_ranges:
        print("none (all kernel indices are populated)")
    else:
        for start, end in empty_ranges:
            n = end - start + 1
            size_tib = n * TIB_PER_PGD_ENTRY
            addr_start = pgd_index_to_addr(start)
            addr_end = pgd_index_to_addr(end) + (1 << PGDIR_SHIFT) - 1
            addrs = f"0x{addr_start:016x} - 0x{addr_end:016x}"
            if start == end:
                print(f"[{start:3d}] {addrs} (Size: {size_tib:g} TiB)")
            else:
                print(f"[{start:3d} - {end:3d}] {addrs} (Size: {n} entries, {size_tib:g} TiB)")

    if not divergent_pids:
        print("all scanned processes share the same non-empty upper PGD indices")
    else:
        print(f"{len(divergent_pids)} processes have a different PGD.")
        for d_pid, diffs in divergent_pids.items():
            print(f"\nPID {d_pid}:")
            if diffs["missing"]:
                print(f"  missing kernel indices: {diffs['missing']}")
            if diffs["extra"]:
                print(f"  extra kernel indices:   {diffs['extra']}")

if __name__ == "__main__":
    main()
