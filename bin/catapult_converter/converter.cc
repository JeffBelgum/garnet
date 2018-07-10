// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "converter.h"

#include <algorithm>
#include <getopt.h>
#include <math.h>
#include <numeric>
#include <vector>

#include "lib/fxl/logging.h"
#include "lib/fxl/random/uuid.h"
#include "lib/fxl/strings/string_printf.h"
#include "third_party/rapidjson/rapidjson/document.h"
#include "third_party/rapidjson/rapidjson/error/en.h"
#include "third_party/rapidjson/rapidjson/filereadstream.h"
#include "third_party/rapidjson/rapidjson/filewritestream.h"
#include "third_party/rapidjson/rapidjson/writer.h"

namespace {

// Calculate the variance, with Bessel's correction applied.
double Variance(const std::vector<double>& values, double mean) {
  double sum_of_squared_diffs = 0.0;
  for (double value : values) {
    double diff = value - mean;
    sum_of_squared_diffs += diff * diff;
  }
  return sum_of_squared_diffs / static_cast<double>(values.size() - 1);
}

void WriteJson(FILE* fp, rapidjson::Document* doc) {
  char buffer[100];
  rapidjson::FileWriteStream output_stream(fp, buffer, sizeof(buffer));
  rapidjson::Writer<rapidjson::FileWriteStream> writer(output_stream);
  doc->Accept(writer);
}

// rapidjson's API is rather verbose to use.  This class provides some
// convenience wrappers.
class JsonHelper {
 public:
  explicit JsonHelper(rapidjson::Document::AllocatorType& alloc)
      : alloc_(alloc) {}

  rapidjson::Value MakeString(const char* string) {
    rapidjson::Value value;
    value.SetString(string, alloc_);
    return value;
  };

  rapidjson::Value Copy(const rapidjson::Value& value) {
    return rapidjson::Value(value, alloc_);
  }

 private:
  rapidjson::Document::AllocatorType& alloc_;
};

}

void Convert(rapidjson::Document* input, rapidjson::Document* output,
             const ConverterArgs* args) {
  rapidjson::Document::AllocatorType& alloc = output->GetAllocator();
  JsonHelper helper(alloc);
  output->SetArray();

  uint32_t next_dummy_guid = 0;
  auto MakeUuid = [&]() {
    std::string uuid;
    if (args->use_test_guids) {
      uuid = fxl::StringPrintf("dummy_guid_%d", next_dummy_guid++);
    } else {
      uuid = fxl::GenerateUUID();
    }
    return helper.MakeString(uuid.c_str());
  };

  rapidjson::Value diagnostic_map;
  diagnostic_map.SetObject();

  auto AddSharedDiagnostic = [&](const char* key, rapidjson::Value value) {
    rapidjson::Value guid = MakeUuid();

    // Add top-level description.
    rapidjson::Value diagnostic;
    diagnostic.SetObject();
    diagnostic.AddMember("guid", helper.Copy(guid), alloc);
    diagnostic.AddMember("type", "GenericSet", alloc);
    rapidjson::Value values;
    values.SetArray();
    values.PushBack(value, alloc);
    diagnostic.AddMember("values", values, alloc);
    output->PushBack(diagnostic, alloc);

    // Make a reference to the top-level description.
    diagnostic_map.AddMember(helper.MakeString(key), guid, alloc);
  };
  rapidjson::Value timestamp;
  timestamp.SetInt(args->timestamp);
  AddSharedDiagnostic("chromiumCommitPositions", std::move(timestamp));
  AddSharedDiagnostic("benchmarks", helper.MakeString(args->test_suite));
  AddSharedDiagnostic("bots", helper.MakeString(args->bots));
  AddSharedDiagnostic("masters", helper.MakeString(args->masters));

  for (auto& element : input->GetArray()) {
    rapidjson::Value histogram;
    histogram.SetObject();
    histogram.AddMember("name", element["label"], alloc);
    histogram.AddMember("unit", "ms_smallerIsBetter", alloc);
    histogram.AddMember("description", "", alloc);
    histogram.AddMember("diagnostics", helper.Copy(diagnostic_map), alloc);

    rapidjson::Value& samples = element["samples"];
    FXL_CHECK(samples.Size() == 1);
    rapidjson::Value& values = samples[0]["values"];
    std::vector<double> vals;
    vals.reserve(values.Size());
    for (auto& val : values.GetArray()) {
      vals.push_back(val.GetDouble());
    }

    // Check time units and convert if necessary.
    const char* unit = element["unit"].GetString();
    if (strcmp(unit, "nanoseconds") == 0 || strcmp(unit, "ns") == 0) {
      // Convert from nanoseconds to milliseconds.
      for (auto& val : vals) {
        val /= 1e6;
      }
    } else if (!(strcmp(unit, "milliseconds") == 0 ||
                 strcmp(unit, "ms") == 0)) {
      printf("Units not recognized: %s\n", unit);
      exit(1);
    }

    double sum = 0;
    double sum_of_logs = 0;
    for (auto val : vals) {
      sum += val;
      sum_of_logs += log(val);
    }
    double mean = sum / vals.size();
    // meanlogs is the mean of the logs of the values, which is useful for
    // calculating the geometric mean of the values.
    double meanlogs = sum_of_logs / vals.size();
    double min = *std::min_element(vals.begin(), vals.end());
    double max = *std::max_element(vals.begin(), vals.end());
    double variance = Variance(vals, mean);
    rapidjson::Value stats;
    stats.SetArray();
    stats.PushBack(vals.size(), alloc);  // "count" entry.
    stats.PushBack(max, alloc);
    stats.PushBack(meanlogs, alloc);
    stats.PushBack(mean, alloc);
    stats.PushBack(min, alloc);
    stats.PushBack(sum, alloc);
    stats.PushBack(variance, alloc);
    histogram.AddMember("running", stats, alloc);

    histogram.AddMember("guid", MakeUuid(), alloc);
    // This field is redundant with the "count" entry in "running".
    histogram.AddMember("maxNumSampleValues", vals.size(), alloc);
    // Assume for now that we didn't get any NaN values.
    histogram.AddMember("numNans", 0, alloc);

    output->PushBack(histogram, alloc);
  }
}

int ConverterMain(int argc, char** argv) {
  const char* usage =
      "Usage: %s [options]\n"
      "\n"
      "This tool takes results from Fuchsia performance tests (in Fuchsia's "
      "JSON perf test results format) and converts them to the Catapult "
      "Dashboard's JSON HistogramSet format.\n"
      "\n"
      "Options:\n"
      "  --input FILENAME\n"
      "      Input file: perf test results JSON file (required)\n"
      "  --output FILENAME\n"
      "      Output file: Catapult HistogramSet JSON file (default is stdout)\n"
      "\n"
      "The following are required and specify parameters to copy into the "
      "output file:\n"
      "  --execution-timestamp-ms NUMBER\n"
      "  --masters STRING\n"
      "  --test-suite STRING\n"
      "  --bots STRING\n"
      "See README.md for the meanings of these parameters.\n";

  // Parse command line arguments.
  static const struct option opts[] = {
    {"help", no_argument, nullptr, 'h'},
    {"input", required_argument, nullptr, 'i'},
    {"output", required_argument, nullptr, 'o'},
    {"execution-timestamp-ms", required_argument, nullptr, 'e'},
    {"masters", required_argument, nullptr, 'm'},
    {"test-suite", required_argument, nullptr, 't'},
    {"bots", required_argument, nullptr, 'b'},
  };
  ConverterArgs args;
  const char* input_filename = nullptr;
  const char* output_filename = nullptr;
  optind = 1;
  for (;;) {
    int opt = getopt_long(argc, argv, "h", opts, nullptr);
    if (opt < 0)
      break;
    switch (opt) {
      case 'h':
        printf(usage, argv[0]);
        return 0;
      case 'i':
        input_filename = optarg;
        break;
      case 'o':
        output_filename = optarg;
        break;
      case 'e':
        args.timestamp = strtol(optarg, nullptr, 0);
        break;
      case 'm':
        args.masters = optarg;
        break;
      case 't':
        args.test_suite = optarg;
        break;
      case 'b':
        args.bots = optarg;
        break;
    }
  }
  if (optind < argc) {
    fprintf(stderr, "Unrecognized argument: \"%s\"\n", argv[optind]);
    return 1;
  }

  // Check arguments.
  bool failed = false;
  if (!input_filename) {
    fprintf(stderr, "--input argument is required\n");
    failed = true;
  }
  if (!args.timestamp) {
    fprintf(stderr, "--execution-timestamp-ms argument is required\n");
    failed = true;
  }
  if (!args.masters) {
    fprintf(stderr, "--masters argument is required\n");
    failed = true;
  }
  if (!args.test_suite) {
    fprintf(stderr, "--test-suite argument is required\n");
    failed = true;
  }
  if (!args.bots) {
    fprintf(stderr, "--bots argument is required\n");
    failed = true;
  }
  if (failed) {
    fprintf(stderr, "\n");
    fprintf(stderr, usage, argv[0]);
    return 1;
  }

  // Read input file.
  FILE* fp = fopen(input_filename, "r");
  if (!fp) {
    fprintf(stderr, "Failed to open input file, \"%s\"\n", input_filename);
    return 1;
  }
  char buffer[100];
  rapidjson::FileReadStream input_stream(fp, buffer, sizeof(buffer));
  rapidjson::Document input;
  rapidjson::ParseResult parse_result = input.ParseStream(input_stream);
  if (!parse_result) {
    fprintf(stderr, "Failed to parse input file, \"%s\": %s (offset %zd)\n",
            input_filename, rapidjson::GetParseError_En(parse_result.Code()),
            parse_result.Offset());
    return 1;
  }
  fclose(fp);

  rapidjson::Document output;
  Convert(&input, &output, &args);

  // Write output.
  if (output_filename) {
    fp = fopen(output_filename, "w");
    if (!fp) {
      fprintf(stderr, "Failed to open output file, \"%s\"\n", output_filename);
      return 1;
    }
    WriteJson(fp, &output);
    fclose(fp);
  } else {
    WriteJson(stdout, &output);
  }

  return 0;
}
