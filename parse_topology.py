import json
import argparse

def extract_ps_list(json_file, src_gid, dst_gid, qps, output_file):
    try:
        states = []
        with open(json_file, "r") as f:
            data = json.load(f)

        if src_gid not in data:
            raise ValueError(f"src_gid '{src_gid}' not found in JSON.")

        if dst_gid not in data[src_gid]:
            raise ValueError(f"dst_gid '{dst_gid}' not found in JSON under '{src_gid}'.")

        unioned_ps_list = set()

        for obj in data[src_gid][dst_gid]:
            unioned_ps_list.update(obj.get("ps_list", []))

        unioned_ps_list = sorted(unioned_ps_list)

        #TODO: temp solution, will be user defined in the future
        states = ['4'] * len(unioned_ps_list)

        with open(output_file, "w") as f:
            for _ in range(0, qps):
                f.write(f"{','.join(map(str, unioned_ps_list))} {','.join(states)}\n")

        print(f"Unioned ps_list {unioned_ps_list} written to {output_file}")

    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Extract and union ps_list values from topology file.",
                                     epilog="Example usage:\n"
                                            "  python parse_topology.py --json_file plane_db.json --src_gid 1002:13::1 --dst_gid 1002:03::1 --output_file output.txt --qps 3")
    parser.add_argument("--topo_file", required=True, help="Path to the input topology JSON file")
    parser.add_argument("--src_gid", required=True, help="Source GID to look for in JSON")
    parser.add_argument("--dst_gid", required=True, help="Destination GID to look for in JSON")
    parser.add_argument("--output_file", required=True, help="Path to the output text file")
    parser.add_argument("--qps", type=int, required=True, help="Number of QP's")

    args = parser.parse_args()

    extract_ps_list(args.json_file, args.src_gid, args.dst_gid, args.qps, args.output_file)
