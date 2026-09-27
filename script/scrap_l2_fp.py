import csv
import os
import sys

def extract_translation_footprints(root_folder_path, output_csv_path):
  extracted_data = []

  # Recursively walk through the directory structure
  for dirpath, dirnames, filenames in os.walk(root_folder_path):
    # Get the leaf folder name to use as the label
    leaf_folder_label = os.path.basename(os.path.normpath(dirpath))

    for filename in filenames:
      file_path = os.path.join(dirpath, filename)

      try:
        with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
          lines = f.readlines()

        # Search for the target header line
        for i in range(len(lines) - 1):
          current_line = lines[i].strip()

          # Match the header pattern flexibly
          if (
              "Core_0_L2C_translation_footprint" in current_line
              and "1,2,3,4,5,6,7,8" in current_line
          ):
            # The line *next* to it holds the values
            next_line = lines[i + 1].strip()
            values = [v.strip() for v in next_line.split(",")]

            extracted_data.append(
                {
                    "label": leaf_folder_label,
                    "filename": filename,
                    "values": values,
                }
            )
            break  # Move to the next file once found

      except Exception as e:
        print(f"Skipping file {file_path} due to error: {e}")

  # Write the aggregated results into a structured CSV file
  with open(output_csv_path, "w", newline="", encoding="utf-8") as csv_file:
    writer = csv.writer(csv_file)

    # Header row (Label, File, and 8 value columns)
    writer.writerow(
        [
            "Label",
            "FileName",
            "Val_1",
            "Val_2",
            "Val_3",
            "Val_4",
            "Val_5",
            "Val_6",
            "Val_7",
            "Val_8",
        ]
    )

    for item in extracted_data:
      row = [item["label"], item["filename"]] + item["values"]
      writer.writerow(row)

  print(
      f"Successfully processed {len(extracted_data)} files. Output saved to"
      f" '{output_csv_path}'."
  )


# --- Example Usage ---
extract_translation_footprints(sys.argv[1], 'summary_footprints.csv')