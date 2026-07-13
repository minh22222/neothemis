#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neothemis {

using CsvRow = std::vector<std::string>;
using CsvTable = std::vector<CsvRow>;
using CsvKey = std::pair<std::string, std::string>;
using CsvLockedMergeCallback = std::function<void(const CsvTable&)>;

CsvTable parse_csv(std::string_view text);
CsvTable read_csv_file(const std::filesystem::path& path);
CsvTable read_csv_file_locked(const std::filesystem::path& path);
std::string escape_csv_field(std::string_view value);
void write_csv_row(std::ostream& output, const CsvRow& row);
void write_text_file_atomic(const std::filesystem::path& path, std::string_view contents);
CsvTable replace_csv_rows_by_key_atomic(const std::filesystem::path& path,
                                        const CsvRow& header,
                                        const CsvTable& replacement_rows,
                                        const std::set<CsvKey>& replaced_keys,
                                        const CsvLockedMergeCallback& while_locked = {});

} // namespace neothemis
