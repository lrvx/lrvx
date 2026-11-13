// node/src/heatmap.h — heatmap renderer wrapper for Node.js

#pragma once

#include <napi.h>

#include "error_translator.h"
#include "lrvx/capi/lrvx_capi.h"

#include <string>
#include <vector>

namespace node_lrvx
{

// lrvx.report.heatmapHtml(z, opts?) → string
// z is a 2D number[][]. opts: { rowLabels?, colLabels?, title?,
//   xAxisName?, yAxisName?, metricName? }.
inline Napi::Value heatmapHtml(const Napi::CallbackInfo& info)
{
  return tryLrvx(info.Env(), [&]() -> Napi::Value
                 {
    auto env = info.Env();
    if (info.Length() == 0 || !info[0].IsArray())
    {
      throw lrvx::LrvxError(
          "E_LEN_002",
          "heatmapHtml: first argument must be a 2D array of numbers.");
    }
    auto outer = argArray(info[0], "report.heatmapHtml: z");
    const uint32_t rows = outer.Length();
    if (rows == 0)
    {
      throw lrvx::LrvxError("E_LEN_002", "heatmapHtml: z is empty.");
    }
    auto firstRow = argArray(outer.Get(uint32_t(0)), "heatmapHtml: a row of z");
    const uint32_t cols = firstRow.Length();
    if (cols == 0)
    {
      throw lrvx::LrvxError("E_LEN_002", "heatmapHtml: z rows are empty.");
    }
    std::vector<double> z(rows * cols);
    for (uint32_t r = 0; r < rows; ++r)
    {
      auto row = argArray(outer.Get(r), "heatmapHtml: a row of z");
      if (row.Length() != cols)
      {
        throw lrvx::LrvxError(
            "E_LEN_001",
            "heatmapHtml: all rows in z must have the same length.");
      }
      for (uint32_t c = 0; c < cols; ++c)
      {
        z[r * cols + c] = argNumber(row.Get(c), "heatmapHtml: a cell of z");
      }
    }

    auto getStringArr = [&](Napi::Object opts, const char* key,
                            std::vector<std::string>& out)
    {
      auto val = opts.Get(key);
      if (given(val))
      {
        auto arr = argArray(val, (std::string("heatmapHtml: ") + key).c_str());
        for (uint32_t i = 0; i < arr.Length(); ++i)
        {
          out.push_back(argString(arr.Get(i), (std::string("heatmapHtml: an entry of ") + key).c_str()));
        }
      }
    };
    auto getString = [&](Napi::Object opts, const char* key) -> std::string
    {
      auto val = opts.Get(key);
      return given(val) ? argString(val, (std::string("heatmapHtml: ") + key).c_str()) : "";
    };

    std::vector<std::string> rowLabels, colLabels;
    std::string title, xAxis, yAxis, metric;
    if (given(info[1]))
    {
      auto opts = argObject(info[1], "report.heatmapHtml: opts");
      getStringArr(opts, "rowLabels", rowLabels);
      getStringArr(opts, "colLabels", colLabels);
      title = getString(opts, "title");
      xAxis = getString(opts, "xAxisName");
      yAxis = getString(opts, "yAxisName");
      metric = getString(opts, "metricName");
    }

    std::vector<const char*> rowLabelPtrs;
    rowLabelPtrs.reserve(rowLabels.size());
    for (const auto& s : rowLabels){ rowLabelPtrs.push_back(s.c_str());
}
    std::vector<const char*> colLabelPtrs;
    colLabelPtrs.reserve(colLabels.size());
    for (const auto& s : colLabels){ colLabelPtrs.push_back(s.c_str());
}

    LrvxHeatmapData d{};
    d.z = z.data();
    d.rows = rows;
    d.cols = cols;
    d.row_labels = rowLabelPtrs.empty() ? nullptr : rowLabelPtrs.data();
    d.num_row_labels = static_cast<uint32_t>(rowLabelPtrs.size());
    d.col_labels = colLabelPtrs.empty() ? nullptr : colLabelPtrs.data();
    d.num_col_labels = static_cast<uint32_t>(colLabelPtrs.size());
    d.title = title.c_str();
    d.x_axis_name = xAxis.c_str();
    d.y_axis_name = yAxis.c_str();
    d.metric_name = metric.c_str();

    uint64_t total = lrvx_render_heatmap_html(&d, nullptr, 0);
    std::string buf(total, '\0');
    lrvx_render_heatmap_html(&d, buf.data(), total);
    return Napi::String::New(env, buf); });
}

inline void registerHeatmap(Napi::Env env, Napi::Object exports)
{
  // Group under exports.report.{heatmapHtml}
  Napi::Object report;
  if (exports.Has("report") && exports.Get("report").IsObject())
  {
    report = exports.Get("report").As<Napi::Object>();
  }
  else
  {
    report = Napi::Object::New(env);
    exports.Set("report", report);
  }
  report.Set("heatmapHtml",
             Napi::Function::New(env, &heatmapHtml, "heatmapHtml"));
}

}  // namespace node_lrvx
