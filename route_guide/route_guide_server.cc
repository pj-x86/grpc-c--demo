/*
 *
 * Copyright 2015 gRPC authors.
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
 *
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include <signal.h>

#include "userlog.h"

#include <grpc/grpc.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/security/server_credentials.h>
#include "helper.h"
#include "log_interceptor_server.h"

#include "route_guide.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using routeguide::Point;
using routeguide::Feature;
using routeguide::Rectangle;
using routeguide::RouteSummary;
using routeguide::RouteNote;
using routeguide::RouteGuide;
using std::chrono::system_clock;

// 专门处理 SIGUSR1 信号
static void signal_handler(int signum)
{
  std::cout << "收到信号: " << signum << std::endl;

  // 重新读取配置文件中的日志级别
  //TODO

  //动态修改日志级别
  modify_log_level("info");
}

float ConvertToRadians(float num) {
  return num * 3.1415926 /180;
}

// The formula is based on http://mathforum.org/library/drmath/view/51879.html
float GetDistance(const Point& start, const Point& end) {
  const float kCoordFactor = 10000000.0;
  float lat_1 = start.latitude() / kCoordFactor;
  float lat_2 = end.latitude() / kCoordFactor;
  float lon_1 = start.longitude() / kCoordFactor;
  float lon_2 = end.longitude() / kCoordFactor;
  float lat_rad_1 = ConvertToRadians(lat_1);
  float lat_rad_2 = ConvertToRadians(lat_2);
  float delta_lat_rad = ConvertToRadians(lat_2-lat_1);
  float delta_lon_rad = ConvertToRadians(lon_2-lon_1);

  float a = pow(sin(delta_lat_rad/2), 2) + cos(lat_rad_1) * cos(lat_rad_2) *
            pow(sin(delta_lon_rad/2), 2);
  float c = 2 * atan2(sqrt(a), sqrt(1-a));
  int R = 6371000; // metres

  return R * c;
}

std::string GetFeatureName(const Point& point,
                           const std::vector<Feature>& feature_list) {
  for (const Feature& f : feature_list) {
    if (f.location().latitude() == point.latitude() &&
        f.location().longitude() == point.longitude()) {
      //std::cout << "found. name=" << f.name() << std::endl;
      SPDLOG_INFO("found. name={}", f.name());
      return f.name();
    }
  }
  return "";
}

class RouteGuideImpl final : public RouteGuide::Service {
 public:
  explicit RouteGuideImpl(const std::string& db) {
    routeguide::ParseDb(db, &feature_list_);
  }

  Status GetFeature(ServerContext* context, const Point* point,
                    Feature* feature) override {
    //std::cout << "latitude=" << point->latitude() << ",longitude=" << point->longitude() << std::endl; 
    SPDLOG_INFO("latitude={:d},longitude={:d}", point->latitude(), point->longitude());
    feature->set_name(GetFeatureName(*point, feature_list_));
    feature->mutable_location()->CopyFrom(*point);
    return Status::OK;
    //return grpc::Status(grpc::StatusCode::NOT_FOUND, "test-not-found");
  }

  Status ListFeatures(ServerContext* context,
                      const routeguide::Rectangle* rectangle,
                      ServerWriter<Feature>* writer) override {
    auto lo = rectangle->lo();
    auto hi = rectangle->hi();
    long left = (std::min)(lo.longitude(), hi.longitude());
    long right = (std::max)(lo.longitude(), hi.longitude());
    long top = (std::max)(lo.latitude(), hi.latitude());
    long bottom = (std::min)(lo.latitude(), hi.latitude());
    for (const Feature& f : feature_list_) {
      if (f.location().longitude() >= left &&
          f.location().longitude() <= right &&
          f.location().latitude() >= bottom &&
          f.location().latitude() <= top) {
        writer->Write(f);
      }
    }
    return Status::OK;
  }

  Status RecordRoute(ServerContext* context, ServerReader<Point>* reader,
                     RouteSummary* summary) override {
    Point point;
    int point_count = 0;
    int feature_count = 0;
    float distance = 0.0;
    Point previous;

    system_clock::time_point start_time = system_clock::now();
    while (reader->Read(&point)) {
      point_count++;
      if (!GetFeatureName(point, feature_list_).empty()) {
        feature_count++;
      }
      if (point_count != 1) {
        distance += GetDistance(previous, point);
      }
      previous = point;
    }
    system_clock::time_point end_time = system_clock::now();
    summary->set_point_count(point_count);
    summary->set_feature_count(feature_count);
    summary->set_distance(static_cast<long>(distance));
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        end_time - start_time);
    summary->set_elapsed_time(secs.count());

    return Status::OK;
  }

  Status RouteChat(ServerContext* context,
                   ServerReaderWriter<RouteNote, RouteNote>* stream) override {
    RouteNote note;
    while (stream->Read(&note)) {
      std::unique_lock<std::mutex> lock(mu_);
      for (const RouteNote& n : received_notes_) {
        if (n.location().latitude() == note.location().latitude() &&
            n.location().longitude() == note.location().longitude()) {
          stream->Write(n);
        }
      }
      received_notes_.push_back(note);
    }

    return Status::OK;
  }

 private:
  std::vector<Feature> feature_list_;
  std::mutex mu_;
  std::vector<RouteNote> received_notes_;
};

void RunServer(const std::string& db_path) {
  std::string server_address("0.0.0.0:50051");
  RouteGuideImpl service(db_path);

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  // 创建服务端拦截器
  std::vector<
    std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>>
      interceptor_creators;
  interceptor_creators.push_back(
    std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>(
      new ServerLoggingInterceptorFactory()));
  builder.experimental().SetInterceptorCreators(std::move(interceptor_creators));

  std::unique_ptr<Server> server(builder.BuildAndStart());
  //std::cout << "Server listening on " << server_address << std::endl;
  SPDLOG_INFO("Server listening on {}", server_address);
 
  server->Wait();
}

int main(int argc, char** argv) {
  // 读取配置文件

  // 初始化日志框架
  init_logger("dev","logs/server","debug");

  //设置信号处理函数，专门处理 SIGUSR1，用于重新读取配置文件日志级别
  struct sigaction sa;

  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART; /* Restart functions if interrupted by handler */
  if (sigaction(SIGUSR1, &sa, NULL) == -1)
      /* Handle error */;

  // Expect only arg: --db_path=route_guide_db.json.
  if (argc < 2) {
    //std::cout << "请先指定参数: --db_path=xxx.json" << std::endl;
    //std::cout << "示例: --db_path=../../route_guide_db.json" << std::endl;
    SPDLOG_ERROR("请先指定参数: --db_path=xxx.json");
    SPDLOG_ERROR("示例: --db_path=../../route_guide_db.json");
    exit(-1);
  }
  std::string db = routeguide::GetDbFileContent(argc, argv);
  RunServer(db);

  //退出日志框架
  exit_logger();

  return 0;
}
