#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "MPC.h"
#include "json.hpp"

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.rfind("}]");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

// Evaluate a polynomial.
double polyeval(Eigen::VectorXd coeffs, double x) {
  double result = 0.0;
  for (int i = 0; i < coeffs.size(); i++) {
    result += coeffs[i] * pow(x, i);
  }
  return result;
}

// Fit a polynomial. Adapted from
// https://github.com/JuliaMath/Polynomials.jl/blob/master/src/Polynomials.jl#L676-L716
Eigen::VectorXd polyfit(Eigen::VectorXd xvals, Eigen::VectorXd yvals,
                        int order) {
  assert(xvals.size() == yvals.size());
  assert(order >= 1 && order <= xvals.size() - 1);
  Eigen::MatrixXd A(xvals.size(), order + 1);

  for (int i = 0; i < xvals.size(); i++) {
    A(i, 0) = 1.0;
  }

  for (int j = 0; j < xvals.size(); j++) {
    for (int i = 0; i < order; i++) {
      A(j, i + 1) = A(j, i) * xvals(j);
    }
  }

  auto Q = A.householderQr();
  auto result = Q.solve(yvals);
  return result;
}

int main() {
  uWS::Hub h;

  MPC mpc;

  h.onMessage([&mpc](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
                     uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    string sdata = string(data).substr(0, length);
    // cout << sdata << endl;
    if (sdata.size() > 2 && sdata[0] == '4' && sdata[1] == '2') {
      string s = hasData(sdata);
      if (s != "") {
        auto j = json::parse(s);
        string event = j[0].get<string>();
        if (event == "telemetry") {
          // j[1] is the data JSON object

          // `ptsx` (Array<float>) - The global x positions of the waypoints.
          // `ptsy` (Array<float>) - The global y positions of the waypoints.
          // This corresponds to the z coordinate in Unity since
          // y is the up-down direction.
          vector<double> ptsx = j[1]["ptsx"];
          vector<double> ptsy = j[1]["ptsy"];

          // `x` (float) - The global x position of the vehicle.
          double px = j[1]["x"];
          // `y` (float) - The global y position of the vehicle.
          double py = j[1]["y"];

          // `psi` (float) - The orientation of the vehicle
          // in radians converted from the Unity format to the standard
          // format expected in most mathemetical functions (more details
          // below).
          // `psi_unity` (float) - The orientation of the vehicle in
          // radians. This is an orientation commonly used in navigation
          // (https://en.wikipedia.org/wiki/Polar_coordinate_system#Position_and_navigation).
          double psi = j[1]["psi"];

          // `speed` (float) - The current velocity in mph.
          double v = j[1]["speed"];

          // `steering_angle` (float) - The current steering angle in radians.
          double steering = j[1]["steering_angle"];
          // `throttle` (float) - The current throttle value [-1, 1].
          double throttle = j[1]["throttle"];

          // positions in the vehicle space
          // i.e. where the vehicle is located at the origin (0, 0)
          // and the orientation is 0/360 degrees
          Eigen::VectorXd car_ptsx = Eigen::VectorXd::Zero(ptsx.size());
          Eigen::VectorXd car_ptsy = Eigen::VectorXd::Zero(ptsy.size());

          for (unsigned int i = 0; i < ptsx.size(); ++i) {
            // transformation of global (x,y) position into the vehicle space
            double x = ptsx[i] - px;
            double y = ptsy[i] - py;
            car_ptsx[i] = x * cos(-psi) - y * sin(-psi);
            car_ptsy[i] = x * sin(-psi) + y * cos(-psi);
          }

          auto coeffs = polyfit(car_ptsx, car_ptsy, 3);

          // The cross track error is calculated by evaluating at polynomial at
          // x = 0, f(x) and subtracting y = 0.
          double cte = polyeval(coeffs, 0);
          // Due to the sign starting at 0, the orientation error is -f'(x).
          // derivative of
          // coeffs[0] + coeffs[1] * x + coeffs[2] * x^2 + coeffs[3] * x^3 ->
          // coeffs[1] evaluated at x = 0
          double epsi = -atan(coeffs[1]);

          // Latency
          // The purpose is to mimic real driving conditions where
          // the car does actuate the commands instantly.
          double latency = 0.1;
          // we need to account for velocity unit of measure conversion:
          // miles per hour to metre per second
          latency *= 0.44704;

          const double Lf = 2.67;
          // updated state variables when latency is included
          px = v * latency;
          py = 0;
          psi = -v * steering * latency / Lf;
          epsi += psi;
          cte += v * latency * sin(epsi);
          // throttle is used as an approximation of the acceleration
          v += throttle * latency;

          Eigen::VectorXd state(6);
          // adding latency to the state vector convert
          // state << 0, 0, 0, v, cte, epsi;
          // into
          state << px, py, psi, v, cte, epsi;

          // Calculate optimal steering angle and throttle using MPC
          auto vars = mpc.Solve(state, coeffs);

          cout << "Vars: " << vars[0] << " " << vars[1] << endl;

          double steer_value = -vars[0] / deg2rad(25);
          double throttle_value = vars[1];

          json msgJson;
          // We divide by deg2rad(25) before sending the
          // steering value back. Otherwise the values will be in between
          // [-deg2rad(25), deg2rad(25] instead of [-1, 1].
          msgJson["steering_angle"] = steer_value;
          msgJson["throttle"] = throttle_value;

          // Display the MPC predicted trajectory
          vector<double> mpc_x_vals;
          vector<double> mpc_y_vals;
          size_t t = 2;
          while (t < vars.size()) {
            mpc_x_vals.push_back(vars[t]);
            mpc_y_vals.push_back(vars[t + 1]);
            t += 2;
          }
          // ... add (x,y) points to list here, points are in reference to the
          // vehicle's coordinate system
          // the points in the simulator are connected by a Green line
          msgJson["mpc_x"] = mpc_x_vals;
          msgJson["mpc_y"] = mpc_y_vals;

          // Display the waypoints/reference line the car has to follow
          vector<double> next_x_vals;
          vector<double> next_y_vals;
          for (unsigned i = 0; i < car_ptsx.size(); ++i) {
            next_x_vals.push_back(car_ptsx[i]);
            next_y_vals.push_back(car_ptsy[i]);
          }
          // ... add (x,y) points to list here, points are in reference to the
          // vehicle's coordinate system
          // the points in the simulator are connected by a Yellow line

          msgJson["next_x"] = next_x_vals;
          msgJson["next_y"] = next_y_vals;

          auto msg = "42[\"steer\"," + msgJson.dump() + "]";
          // std::cout << msg << std::endl;
          this_thread::sleep_for(chrono::milliseconds(100));
          ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}
