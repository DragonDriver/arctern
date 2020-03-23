/*
 * Copyright (C) 2019-2020 Zilliz. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <iostream>
#include <ogr_api.h>
#include <ogrsf_frmts.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

#include "render/render_builder.h"

#include "arrow/render_api.h"

namespace arctern {
namespace render {

std::shared_ptr<arrow::Array> out_pic(std::pair<uint8_t*, int64_t> output) {
  if (output.first == nullptr || output.second < 0) {
    // TODO: add log here
    return nullptr;
  }

  auto output_length = output.second;
  auto output_data = output.first;
  auto bit_map = (uint8_t*)malloc(output_length);
  memset(bit_map, 0xff, output_length);

  auto buffer0 = std::make_shared<arrow::Buffer>(bit_map, output_length);
  auto buffer1 = std::make_shared<arrow::Buffer>(output_data, output_length);
  auto buffers = std::vector<std::shared_ptr<arrow::Buffer>>();
  buffers.emplace_back(buffer0);
  buffers.emplace_back(buffer1);

  auto data_type = arrow::uint8();
  auto array_data = arrow::ArrayData::Make(data_type, output_length, buffers);
  auto array = arrow::MakeArray(array_data);
  return array;
}

std::shared_ptr<arrow::Array> WktToWkb(const std::shared_ptr<arrow::Array>& arr_wkt) {
  auto wkts = std::static_pointer_cast<arrow::StringArray>(arr_wkt);
  auto wkt_size = arr_wkt->length();
  auto wkt_type = arr_wkt->type_id();
  assert(wkt_type == arrow::Type::STRING);

  arrow::BinaryBuilder builder;
  for (int i = 0; i < wkt_size; i++) {
    auto wkt = wkts->GetString(i);
    OGRGeometry* geo = nullptr;
    CHECK_GDAL(OGRGeometryFactory::createFromWkt(wkt.c_str(), nullptr, &geo));
    assert(err_code == OGRERR_NONE);
    auto sz = geo->WkbSize();
    std::vector<char> wkb(sz);
    CHECK_GDAL(geo->exportToWkb(OGRwkbByteOrder::wkbNDR, (uint8_t*)wkb.data()));
    assert(err_code == OGRERR_NONE);
    OGRGeometryFactory::destroyGeometry(geo);
    auto st = builder.Append(wkb.data(), wkb.size());
    assert(st.ok());
  }
  std::shared_ptr<arrow::Array> result;
  auto st = builder.Finish(&result);
  assert(st.ok());
  return result;
}

template <typename T>
std::pair<uint8_t*, int64_t> render_heatmap(const std::shared_ptr<arrow::Array>& points,
                                       const std::shared_ptr<arrow::Array>& arr_c,
                                       const std::string& conf) {
  auto data = weight_agg<T>(points, arr_c);
  auto num_point = data.size();
  std::vector<uint32_t> input_x(num_point);
  std::vector<uint32_t> input_y(num_point);
  std::vector<T> input_c(num_point);
  auto ite1 = data.begin();
  auto ite2 = data.end();
  std::size_t i = 0;
  for (; ite1 != ite2;) {
    auto geo = ite1->first;
    auto rst_pointer = reinterpret_cast<OGRPoint*>(geo);
    input_x[i] = rst_pointer->getX();
    input_y[i] = rst_pointer->getY();
    input_c[i++] = ite1->second;
    OGRGeometryFactory::destroyGeometry(geo);
    data.erase(ite1++);
  }
  data.clear();
  return heatmap<T>(&input_x[0], &input_y[0], &input_c[0], num_point, conf);
}

template <typename T>
std::pair<uint8_t*, int64_t> render_choroplethmap(const std::shared_ptr<arrow::Array>& arr_wkb,
                                       const std::shared_ptr<arrow::Array>& arr_c,
                                       const std::string& conf) {
  auto data = weight_agg<T>(arr_wkb, arr_c);
  auto num_geo = data.size();
  std::vector<OGRGeometry*> input_wkb(num_geo);
  std::vector<T> input_c(num_geo);
  std::size_t i = 0;
  for (auto ite1 = data.begin(); ite1 != data.end(); ite1++) {
    input_wkb[i] = ite1->first;
    input_c[i++] = ite1->second;
  }
  auto result = choroplethmap<T>(input_wkb, &input_c[0], num_geo, conf);
  return result;
}

std::shared_ptr<arrow::Array> transform_and_projection(
    const std::shared_ptr<arrow::Array>& geos, const std::string& src_rs,
    const std::string& dst_rs, const std::string& bottom_right,
    const std::string& top_left, const int& height, const int& width) {
  return TransformAndProjection(geos, src_rs, dst_rs, bottom_right, top_left, height,
                                width);
}

std::shared_ptr<arrow::Array> point_map(const std::shared_ptr<arrow::Array>& points,
                                        const std::string& conf) {
  auto point_arr = std::static_pointer_cast<arrow::BinaryArray>(points);
  auto num_point = points->length();
  auto wkb_type = points->type_id();
  assert(wkb_type == arrow::Type::Binary);

  std::vector<uint32_t> input_x(num_point);
  std::vector<uint32_t> input_y(num_point);
  OGRGeometry* res_geo = nullptr;
  for (size_t i = 0; i < num_point; i++) {
    std::string geo_wkb = point_arr->GetString(i);
    CHECK_GDAL(OGRGeometryFactory::createFromWkb(geo_wkb.c_str(), nullptr, &res_geo));
    auto rs_pointer = reinterpret_cast<OGRPoint*>(res_geo);
    input_x[i] = rs_pointer->getX();
    input_y[i] = rs_pointer->getY();
  }
  auto result = pointmap(&input_x[0], &input_y[0], num_point, conf);

  return out_pic(result);
}

std::shared_ptr<arrow::Array> point_map(const std::shared_ptr<arrow::Array>& arr_x,
                                        const std::shared_ptr<arrow::Array>& arr_y,
                                        const std::string& conf) {
  auto x_length = arr_x->length();
  auto y_length = arr_y->length();
  auto x_type = arr_x->type_id();
  auto y_type = arr_y->type_id();
  assert(x_length == y_length);
  assert(x_type == arrow::Type::UINT32);
  assert(y_type == arrow::Type::UINT32);

  auto input_x = (uint32_t*)arr_x->data()->GetValues<uint8_t>(1);
  auto input_y = (uint32_t*)arr_y->data()->GetValues<uint8_t>(1);

  return out_pic(pointmap(input_x, input_y, x_length, conf));
}

std::shared_ptr<arrow::Array> heat_map(const std::shared_ptr<arrow::Array>& points,
                                       const std::shared_ptr<arrow::Array>& arr_c,
                                       const std::string& conf) {
  auto points_arr = std::static_pointer_cast<arrow::BinaryArray>(points);
  auto points_size = points->length();
  auto wkb_type = points->type_id();
  assert(wkb_type == arrow::Type::BINARY);

  std::pair<uint8_t*, int64_t> result;
  auto c_type = arr_c->type_id();
  switch (c_type) {
    case arrow::Type::INT8: {
      result = render_heatmap<int8_t>(points, arr_c, conf);
      break;
    }
    case arrow::Type::INT16: {
      result = render_heatmap<int16_t>(points, arr_c, conf);
      break;
    }
    case arrow::Type::INT32: {
      result = render_heatmap<int32_t>(points, arr_c, conf);
      break;
    }
    case arrow::Type::INT64: {
      result = render_heatmap<int64_t>(points, arr_c, conf);
      break;
    }
    case arrow::Type::UINT8: {
      result = render_heatmap<uint8_t>(points, arr_c, conf);
      break;
    }
    case arrow::Type::UINT16: {
      result = render_heatmap<uint16_t>(points, arr_c, conf);
      break;
    }
    case arrow::Type::UINT32: {
      result = render_heatmap<uint32_t>(points, arr_c, conf);
      break;
    }
    case arrow::Type::UINT64: {
      result = render_heatmap<uint64_t>(points, arr_c, conf);
      break;
    }
    case arrow::Type::FLOAT: {
      result = render_heatmap<float>(points, arr_c, conf);
      break;
    }
    case arrow::Type::DOUBLE: {
      result = render_heatmap<double>(points, arr_c, conf);
      break;
    }
    default:
      // TODO: add log here
      std::cout << "type error! " << std::endl;
  }

  return out_pic(result);
}

std::shared_ptr<arrow::Array> heat_map(const std::shared_ptr<arrow::Array>& arr_x,
                                       const std::shared_ptr<arrow::Array>& arr_y,
                                       const std::shared_ptr<arrow::Array>& arr_c,
                                       const std::string& conf) {
  auto x_length = arr_x->length();
  auto y_length = arr_y->length();
  auto c_length = arr_c->length();
  auto x_type = arr_x->type_id();
  auto y_type = arr_y->type_id();
  auto c_type = arr_c->type_id();
  assert(x_length == y_length);
  assert(x_length == c_length);
  assert(x_type == arrow::Type::UINT32);
  assert(y_type == arrow::Type::UINT32);

  auto input_x = (uint32_t*)arr_x->data()->GetValues<uint8_t>(1);
  auto input_y = (uint32_t*)arr_y->data()->GetValues<uint8_t>(1);

  std::pair<uint8_t*, int64_t> output;
  switch (c_type) {
    case arrow::Type::INT8: {
      auto input_c_int8 = (int8_t*)arr_c->data()->GetValues<uint8_t>(1);
      return out_pic(heatmap<int8_t>(input_x, input_y, input_c_int8, x_length, conf));
    }
    case arrow::Type::INT16: {
      auto input_c_int16 = (int16_t*)arr_c->data()->GetValues<uint8_t>(1);
      return out_pic(heatmap<int16_t>(input_x, input_y, input_c_int16, x_length, conf));
    }
    case arrow::Type::INT32: {
      auto input_c_int32 = (int32_t*)arr_c->data()->GetValues<uint8_t>(1);
      return out_pic(heatmap<int32_t>(input_x, input_y, input_c_int32, x_length, conf));
    }
    case arrow::Type::INT64: {
      auto input_c_int64 = (int64_t*)arr_c->data()->GetValues<uint8_t>(1);
      return out_pic(heatmap<int64_t>(input_x, input_y, input_c_int64, x_length, conf));
    }
    case arrow::Type::UINT8: {
      auto input_c_uint8 = (uint8_t*)arr_c->data()->GetValues<uint8_t>(1);
      return out_pic(heatmap<uint8_t>(input_x, input_y, input_c_uint8, x_length, conf));
    }
    case arrow::Type::UINT16: {
      auto input_c_uint16 = (uint16_t*)arr_c->data()->GetValues<uint8_t>(1);
      return out_pic(heatmap<uint16_t>(input_x, input_y, input_c_uint16, x_length, conf));
    }
    case arrow::Type::UINT32: {
      auto input_c_uint32 = (uint32_t*)arr_c->data()->GetValues<uint8_t>(1);
      return out_pic(heatmap<uint32_t>(input_x, input_y, input_c_uint32, x_length, conf));
    }
    case arrow::Type::UINT64: {
      auto input_c_uint64 = (uint64_t*)arr_c->data()->GetValues<uint8_t>(1);
      return out_pic(heatmap<uint64_t>(input_x, input_y, input_c_uint64, x_length, conf));
    }
    case arrow::Type::FLOAT: {
      auto input_c_float = (float*)arr_c->data()->GetValues<uint8_t>(1);
      return out_pic(heatmap<float>(input_x, input_y, input_c_float, x_length, conf));
    }
    case arrow::Type::DOUBLE: {
      auto input_c_double = (double*)arr_c->data()->GetValues<uint8_t>(1);
      return out_pic(heatmap<double>(input_x, input_y, input_c_double, x_length, conf));
    }
    default:
      // TODO: add log here
      std::cout << "type error! heatmap" << std::endl;
  }
  return nullptr;
}

std::shared_ptr<arrow::Array> choropleth_map(
    const std::shared_ptr<arrow::Array>& arr_wkb,
    const std::shared_ptr<arrow::Array>& arr_c, const std::string& conf) {

  auto geo_arr = std::static_pointer_cast<arrow::BinaryArray>(arr_wkb);
  auto geo_size = arr_wkb->length();
  auto wkb_type = arr_wkb->type_id();
  assert(wkb_type == arrow::Type::BINARY);

  std::pair<uint8_t*, int64_t> result;
  auto c_size = arr_c->length();
  auto c_type = arr_c->type_id();
  assert(geo_size == c_size);
  switch (c_type) {
    case arrow::Type::INT8: {
      result = render_choroplethmap<int8_t>(arr_wkb, arr_c, conf);
      break;
    }
    case arrow::Type::INT16: {
      result = render_choroplethmap<int16_t>(arr_wkb, arr_c, conf);
      break;
    }
    case arrow::Type::INT32: {
      result = render_choroplethmap<int32_t>(arr_wkb, arr_c, conf);
      break;
    }
    case arrow::Type::INT64: {
      result = render_choroplethmap<int64_t>(arr_wkb, arr_c, conf);
      break;
    }
    case arrow::Type::UINT8: {
      result = render_choroplethmap<uint8_t>(arr_wkb, arr_c, conf);
      break;
    }
    case arrow::Type::UINT16: {
      result = render_choroplethmap<uint16_t>(arr_wkb, arr_c, conf);
      break;
    }
    case arrow::Type::UINT32: {
      result = render_choroplethmap<uint32_t>(arr_wkb, arr_c, conf);
      break;
    }
    case arrow::Type::UINT64: {
      result = render_choroplethmap<uint64_t>(arr_wkb, arr_c, conf);
      break;
    }
    case arrow::Type::FLOAT: {
      result = render_choroplethmap<float>(arr_wkb, arr_c, conf);
      break;
    }
    case arrow::Type::DOUBLE: {
      result = render_choroplethmap<double>(arr_wkb, arr_c, conf);
      break;
    }
    default:
      // TODO: add log here
      std::cout << "type error!" << std::endl;
  }
  return out_pic(result);
}

}  // namespace render
}  // namespace arctern
