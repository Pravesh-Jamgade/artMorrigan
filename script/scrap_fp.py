import csv
import os
import sys


def extract_variable_size_same_line(
    root_folder_path,
    output_csv_path="variable_size_footprints.csv",
    target_metric="Core_0_STLB_block_footprint",
):
  extracted_rows = []
  allowed_sizes = {1, 2, 4, 8}

  # Recursively walk through the directory structure
  for dirpath, dirnames, filenames in os.walk(root_folder_path):
    leaf_folder_label = os.path.basename(os.path.normpath(dirpath))

    for filename in filenames:
      file_path = os.path.join(dirpath, filename)

      try:
        with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
          lines = f.readlines()

        for i in range(len(lines) - 1):
          line1 = lines[i].strip()
          line2 = lines[i + 1].strip()

          if line1.startswith(target_metric + ",") and line2.startswith(
              target_metric + ","
          ):
            parts_keys = [p.strip() for p in line1.split(",")]
            parts_vals = [p.strip() for p in line2.split(",")]

            keys = parts_keys[1:]
            values = parts_vals[1:]

            if len(keys) in allowed_sizes and len(values) == len(keys):
              row_data = {
                  "Label": leaf_folder_label,
                  "FileName": filename,
                  "Size": len(keys),
              }

              for idx in range(8):
                col_name = f"Val_{idx+1}"
                row_data[col_name] = values[idx] if idx < len(values) else ""

              extracted_rows.append(row_data)
              break

      except Exception as e:
        print(f"Skipping file {file_path} due to error: {e}")

  with open(output_csv_path, "w", newline="", encoding="utf-8") as csv_file:
    fieldnames = [
        "Label",
        "FileName",
        "Size",
        "Val_1",
        "Val_2",
        "Val_3",
        "Val_4",
        "Val_5",
        "Val_6",
        "Val_7",
        "Val_8",
    ]
    writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
    writer.writeheader()
    for row in extracted_rows:
      writer.writerow(row)

  print(
      f"Successfully extracted {len(extracted_rows)} files into"
      f" '{output_csv_path}'."
  )


if __name__ == "__main__":
  if len(sys.argv) < 2:
    print("Usage: python script.py <folder_path> [output_csv_path]")
    sys.exit(1)

  folder_arg = sys.argv[1]
  output_arg = (
      sys.argv[2] if len(sys.argv) > 2 else "variable_size_footprints.csv"
  )

  extract_variable_size_same_line(folder_arg, output_arg)