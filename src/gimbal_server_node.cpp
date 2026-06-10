/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

extern "C" {
#include <gimbal.h>
}

#include <algorithm>
#include <chrono>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <rclcpp/rclcpp.hpp>

#include <control_gimbal_node/msg/gimbal_euler.hpp>
#include <control_gimbal_node/msg/gimbal_state.hpp>
#include <control_gimbal_node/srv/set_gimbal_limits.hpp>
#include <control_gimbal_node/srv/set_gimbal_mode.hpp>
#include <control_gimbal_node/srv/set_gimbal_target.hpp>
#include <control_gimbal_node/srv/set_gimbal_zoom.hpp>

namespace {

using GimbalEulerMsg = control_gimbal_node::msg::GimbalEuler;
using GimbalStateMsg = control_gimbal_node::msg::GimbalState;

float clampf(float value, float min_value, float max_value)
{
    if (value < min_value) {
    return min_value;
    }
    if (value > max_value) {
    return max_value;
    }
    return value;
}

std::vector<double> normalize_vec_param(
    const std::vector<double> & values, const std::string & name)
{
    if (values.size() == 3U) {
    return values;
    }
    if (values.size() == 1U) {
    return std::vector<double>(3U, values[0]);
    }

    throw std::runtime_error(
            "Parameter '" + name + "' size mismatch: got " + std::to_string(values.size()) +
            ", expected 3 (or 1)");
}

gimbal_euler_t to_native(const GimbalEulerMsg & msg)
{
    gimbal_euler_t out{};
    out.pitch = msg.pitch;
    out.yaw = msg.yaw;
    out.roll = msg.roll;
    return out;
}

GimbalEulerMsg to_msg(const gimbal_euler_t & value)
{
    GimbalEulerMsg msg;
    msg.pitch = value.pitch;
    msg.yaw = value.yaw;
    msg.roll = value.roll;
    return msg;
}

bool is_valid_mode(uint8_t mode)
{
    return mode <= static_cast<uint8_t>(GIMBAL_MODE_CALIBRATE);
}

bool is_valid_zoom(uint8_t direction)
{
    return direction <= static_cast<uint8_t>(GIMBAL_ZOOM_OUT);
}

std::string status_to_string(int status)
{
    switch (status) {
    case GIMBAL_OK:
        return "ok";
    case GIMBAL_ERR_ALLOC:
        return "allocation failed";
    case GIMBAL_ERR_CONNECT:
        return "connection failed";
    case GIMBAL_ERR_TIMEOUT:
        return "timeout";
    case GIMBAL_ERR_CONFIG:
        return "configuration error";
    case GIMBAL_ERR_PARAM:
        return "invalid parameter";
    case GIMBAL_ERR_NOSYS:
        return "operation not supported";
    default:
        return "unknown error";
    }
}

}  // namespace

class GimbalServerNode final : public rclcpp::Node
{
public:
    GimbalServerNode()
    : rclcpp::Node("gimbal_server_node")
    {
    driver_name_ = declare_parameter<std::string>("driver_name", "drv_udp_tz0xxx");
    bind_ip_ = declare_parameter<std::string>("bind_ip", "0.0.0.0");
    bind_port_ = declare_parameter<int>("bind_port", 4900);
    device_ip_ = declare_parameter<std::string>("device_ip", "192.168.44.160");
    device_port_ = declare_parameter<int>("device_port", 4900);
    resend_period_s_ = declare_parameter<double>("resend_period_s", 0.02);
    tick_hz_ = declare_parameter<double>("tick_hz", 50.0);
    state_publish_hz_ = declare_parameter<double>("state_publish_hz", 20.0);
    stable_threshold_deg_ = declare_parameter<double>("stable_threshold_deg", 1.5);
    frame_id_ = declare_parameter<std::string>("frame_id", "base_link");

    use_limits_ = declare_parameter<bool>("use_limits", false);
    min_angle_deg_ = normalize_vec_param(
        declare_parameter<std::vector<double>>("min_angle_deg", {-120.0, -179.0, -180.0}),
        "min_angle_deg");
    max_angle_deg_ = normalize_vec_param(
        declare_parameter<std::vector<double>>("max_angle_deg", {90.0, 179.0, 180.0}),
        "max_angle_deg");
    max_speed_deg_s_ = normalize_vec_param(
        declare_parameter<std::vector<double>>("max_speed_deg_s", {50.0, 50.0, 50.0}),
        "max_speed_deg_s");

    gimbal_udp_config_t cfg{};
    cfg.bind_ip = bind_ip_.c_str();
    cfg.bind_port = static_cast<uint16_t>(bind_port_);
    cfg.device_ip = device_ip_.c_str();
    cfg.device_port = static_cast<uint16_t>(device_port_);
    cfg.resend_period_s = static_cast<float>(resend_period_s_);

    dev_ = gimbal_alloc_udp(driver_name_.c_str(), &cfg);
    if (!dev_) {
        throw std::runtime_error("gimbal_alloc_udp failed");
    }

    if (use_limits_) {
        gimbal_limits_t limits = make_limits_from_params();
        const int rc = gimbal_set_limits(dev_, &limits);
        if (rc != GIMBAL_OK) {
        throw std::runtime_error("gimbal_set_limits failed: " + status_to_string(rc));
        }
        limits_ = limits;
        limits_valid_ = true;
    }

    state_pub_ = create_publisher<GimbalStateMsg>(
        "/gimbal/state", rclcpp::QoS(10).reliable());

    set_mode_srv_ = create_service<control_gimbal_node::srv::SetGimbalMode>(
        "/gimbal/set_mode",
        std::bind(&GimbalServerNode::on_set_mode, this, std::placeholders::_1, std::placeholders::_2));

    set_target_srv_ = create_service<control_gimbal_node::srv::SetGimbalTarget>(
        "/gimbal/set_target",
        std::bind(&GimbalServerNode::on_set_target, this, std::placeholders::_1, std::placeholders::_2));

    set_limits_srv_ = create_service<control_gimbal_node::srv::SetGimbalLimits>(
        "/gimbal/set_limits",
        std::bind(&GimbalServerNode::on_set_limits, this, std::placeholders::_1, std::placeholders::_2));

    set_zoom_srv_ = create_service<control_gimbal_node::srv::SetGimbalZoom>(
        "/gimbal/set_zoom",
        std::bind(&GimbalServerNode::on_set_zoom, this, std::placeholders::_1, std::placeholders::_2));

    const auto tick_period = std::chrono::duration<double>(1.0 / std::max(1.0, tick_hz_));
    tick_timer_ = create_wall_timer(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tick_period),
        std::bind(&GimbalServerNode::tick, this));

    const auto publish_period = std::chrono::duration<double>(1.0 / std::max(1.0, state_publish_hz_));
    state_timer_ = create_wall_timer(
        std::chrono::duration_cast<std::chrono::nanoseconds>(publish_period),
        std::bind(&GimbalServerNode::publish_state, this));

    last_tick_tp_ = std::chrono::steady_clock::now();

    RCLCPP_INFO(
        get_logger(),
        "gimbal_server_node ready: driver=%s local=%s:%d remote=%s:%d",
        driver_name_.c_str(), bind_ip_.c_str(), bind_port_, device_ip_.c_str(), device_port_);
    }

    ~GimbalServerNode() override
    {
    if (dev_) {
        gimbal_free(dev_);
        dev_ = nullptr;
    }
    }

private:
    template<typename ResponseT>
    void fill_response(
    const std::shared_ptr<ResponseT> & response,
    int status,
    const std::string & detail = std::string())
    {
    response->success = (status == GIMBAL_OK);
    response->status_code = status;
    response->message = detail.empty() ? status_to_string(status) : detail;
    }

    gimbal_limits_t make_limits_from_params() const
    {
    gimbal_limits_t limits{};
    limits.min_angle.pitch = static_cast<float>(min_angle_deg_[0]);
    limits.min_angle.yaw = static_cast<float>(min_angle_deg_[1]);
    limits.min_angle.roll = static_cast<float>(min_angle_deg_[2]);
    limits.max_angle.pitch = static_cast<float>(max_angle_deg_[0]);
    limits.max_angle.yaw = static_cast<float>(max_angle_deg_[1]);
    limits.max_angle.roll = static_cast<float>(max_angle_deg_[2]);
    limits.max_speed.pitch = static_cast<float>(max_speed_deg_s_[0]);
    limits.max_speed.yaw = static_cast<float>(max_speed_deg_s_[1]);
    limits.max_speed.roll = static_cast<float>(max_speed_deg_s_[2]);
    return limits;
    }

    gimbal_euler_t clamp_target(gimbal_mode_t mode, const gimbal_euler_t & target) const
    {
    if (!limits_valid_) {
        return target;
    }

    gimbal_euler_t clamped = target;
    if (mode == GIMBAL_MODE_SPEED) {
        clamped.pitch = clampf(clamped.pitch, -std::fabs(limits_.max_speed.pitch), std::fabs(limits_.max_speed.pitch));
        clamped.yaw = clampf(clamped.yaw, -std::fabs(limits_.max_speed.yaw), std::fabs(limits_.max_speed.yaw));
        clamped.roll = clampf(clamped.roll, -std::fabs(limits_.max_speed.roll), std::fabs(limits_.max_speed.roll));
        return clamped;
    }

    clamped.pitch = clampf(clamped.pitch, limits_.min_angle.pitch, limits_.max_angle.pitch);
    clamped.yaw = clampf(clamped.yaw, limits_.min_angle.yaw, limits_.max_angle.yaw);
    clamped.roll = clampf(clamped.roll, limits_.min_angle.roll, limits_.max_angle.roll);
    return clamped;
    }

    void update_mode_cache(gimbal_mode_t mode)
    {
    current_mode_ = mode;
    if (mode == GIMBAL_MODE_SPEED) {
        last_target_ = gimbal_euler_t{};
        has_target_ = false;
    } else if (mode != GIMBAL_MODE_ANGLE_ABS && mode != GIMBAL_MODE_ANGLE_REL) {
        has_target_ = false;
    }
    }

    void tick()
    {
    const auto now_tp = std::chrono::steady_clock::now();
    const auto dt = std::chrono::duration<double>(now_tp - last_tick_tp_).count();
    last_tick_tp_ = now_tp;
    gimbal_tick(dev_, static_cast<float>(dt > 0.0 ? dt : 0.0));
    }

    void publish_state()
    {
    gimbal_euler_t angle{};
    gimbal_euler_t speed{};
    const int rc = gimbal_get_state(dev_, &angle, &speed);

    GimbalStateMsg msg;
    msg.header.stamp = now();
    msg.header.frame_id = frame_id_;
    msg.mode = static_cast<uint8_t>(current_mode_);
    msg.has_feedback = (rc == GIMBAL_OK);
    msg.has_target = has_target_;
    msg.stable = gimbal_is_stable(dev_, static_cast<float>(stable_threshold_deg_));
    msg.status_code = rc;
    msg.angle = to_msg(angle);
    msg.speed = to_msg(speed);
    msg.target = to_msg(last_target_);
    state_pub_->publish(msg);
    }

    void on_set_mode(
    const std::shared_ptr<control_gimbal_node::srv::SetGimbalMode::Request> request,
    std::shared_ptr<control_gimbal_node::srv::SetGimbalMode::Response> response)
    {
    if (!is_valid_mode(request->mode)) {
        fill_response(response, GIMBAL_ERR_PARAM, "invalid gimbal mode");
        return;
    }

    const auto mode = static_cast<gimbal_mode_t>(request->mode);
    const int rc = gimbal_set_mode(dev_, mode);
    if (rc == GIMBAL_OK) {
        update_mode_cache(mode);
    }
    fill_response(response, rc);
    }

    void on_set_target(
    const std::shared_ptr<control_gimbal_node::srv::SetGimbalTarget::Request> request,
    std::shared_ptr<control_gimbal_node::srv::SetGimbalTarget::Response> response)
    {
    const gimbal_euler_t requested = to_native(request->target);
    gimbal_euler_t cached_target = requested;
    if (current_mode_ == GIMBAL_MODE_ANGLE_REL) {
        gimbal_euler_t angle{};
        if (gimbal_get_state(dev_, &angle, nullptr) == GIMBAL_OK) {
        cached_target.pitch += angle.pitch;
        cached_target.yaw += angle.yaw;
        cached_target.roll += angle.roll;
        }
    }
    cached_target = clamp_target(current_mode_, cached_target);

    const int rc = gimbal_set_target(dev_, &requested);
    if (rc == GIMBAL_OK) {
        last_target_ = cached_target;
        has_target_ = true;
    }
    fill_response(response, rc);
    }

    void on_set_limits(
    const std::shared_ptr<control_gimbal_node::srv::SetGimbalLimits::Request> request,
    std::shared_ptr<control_gimbal_node::srv::SetGimbalLimits::Response> response)
    {
    gimbal_limits_t limits{};
    limits.min_angle = to_native(request->min_angle);
    limits.max_angle = to_native(request->max_angle);
    limits.max_speed = to_native(request->max_speed);

    const int rc = gimbal_set_limits(dev_, &limits);
    if (rc == GIMBAL_OK) {
        limits_ = limits;
        limits_valid_ = true;
        use_limits_ = true;
    }
    fill_response(response, rc);
    }

    void on_set_zoom(
    const std::shared_ptr<control_gimbal_node::srv::SetGimbalZoom::Request> request,
    std::shared_ptr<control_gimbal_node::srv::SetGimbalZoom::Response> response)
    {
    if (!is_valid_zoom(request->direction)) {
        fill_response(response, GIMBAL_ERR_PARAM, "invalid zoom direction");
        return;
    }

    const int rc = gimbal_set_zoom(
        dev_, static_cast<gimbal_zoom_dir_t>(request->direction), request->speed_level);
    fill_response(response, rc);
    }

    std::string driver_name_;
    std::string bind_ip_;
    int bind_port_{4900};
    std::string device_ip_;
    int device_port_{4900};
    double resend_period_s_{0.02};
    double tick_hz_{50.0};
    double state_publish_hz_{20.0};
    double stable_threshold_deg_{1.5};
    std::string frame_id_{"base_link"};

    bool use_limits_{false};
    std::vector<double> min_angle_deg_;
    std::vector<double> max_angle_deg_;
    std::vector<double> max_speed_deg_s_;

    gimbal_dev * dev_{nullptr};
    gimbal_mode_t current_mode_{GIMBAL_MODE_OFF};
    gimbal_euler_t last_target_{};
    bool has_target_{false};
    gimbal_limits_t limits_{};
    bool limits_valid_{false};
    std::chrono::steady_clock::time_point last_tick_tp_{};

    rclcpp::Publisher<GimbalStateMsg>::SharedPtr state_pub_;
    rclcpp::Service<control_gimbal_node::srv::SetGimbalMode>::SharedPtr set_mode_srv_;
    rclcpp::Service<control_gimbal_node::srv::SetGimbalTarget>::SharedPtr set_target_srv_;
    rclcpp::Service<control_gimbal_node::srv::SetGimbalLimits>::SharedPtr set_limits_srv_;
    rclcpp::Service<control_gimbal_node::srv::SetGimbalZoom>::SharedPtr set_zoom_srv_;
    rclcpp::TimerBase::SharedPtr tick_timer_;
    rclcpp::TimerBase::SharedPtr state_timer_;
};

int main(int argc, char ** argv)
{
    rclcpp::init(argc, argv);
    try {
    rclcpp::spin(std::make_shared<GimbalServerNode>());
    } catch (const std::exception & e) {
    std::fprintf(stderr, "gimbal_server_node exception: %s\n", e.what());
    }
    rclcpp::shutdown();
    return 0;
}
