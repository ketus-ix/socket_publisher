#include "socket_publisher/data_serializer.h"

#include "stella_vslam/data/keyframe.h"
#include "stella_vslam/data/landmark.h"
#include "stella_vslam/publish/frame_publisher.h"
#include "stella_vslam/publish/map_publisher.h"

#include <forward_list>

#include <opencv2/imgcodecs.hpp>

// map_segment.pb.h will be generated into build/src/socket_publisher/ when make
#include "map_segment.pb.h"

namespace socket_publisher {

std::string data_serializer::serialized_reset_signal_{};

data_serializer::data_serializer(const std::shared_ptr<stella_vslam::publish::frame_publisher>& frame_publisher,
                                 const std::shared_ptr<stella_vslam::publish::map_publisher>& map_publisher,
                                 bool publish_points)
    : frame_publisher_(frame_publisher), map_publisher_(map_publisher), publish_points_(publish_points),
      keyframe_hash_map_(new std::unordered_map<unsigned int, double>), point_hash_map_(new std::unordered_map<unsigned int, double>) {
    const auto tags = std::vector<std::string>{"RESET_ALL"};
    const auto messages = std::vector<std::string>{"reset all data"};
    data_serializer::serialized_reset_signal_ = serialize_messages(tags, messages);
}

std::string data_serializer::serialize_messages(const std::vector<std::string>& tags, const std::vector<std::string>& messages) {
    unsigned int length = std::min(tags.size(), messages.size());

    map_segment::map map;

    for (unsigned int i = 0; i < length; i++) {
        auto message = map.add_messages();
        message->set_tag(tags.at(i));
        message->set_txt(messages.at(i));
    }

    std::string buffer;
    map.SerializeToString(&buffer);

    const auto* cstr = reinterpret_cast<const unsigned char*>(buffer.c_str());
    return base64_encode(cstr, buffer.length());
}

std::string data_serializer::serialize_map_diff() {
    std::vector<std::shared_ptr<stella_vslam::data::keyframe>> keyframes;
    map_publisher_->get_keyframes(keyframes);

    std::vector<std::shared_ptr<stella_vslam::data::landmark>> all_landmarks;
    std::set<std::shared_ptr<stella_vslam::data::landmark>> local_landmarks;
    if (publish_points_) {
        map_publisher_->get_landmarks(all_landmarks, local_landmarks);
    }

    const auto current_camera_pose = map_publisher_->get_current_cam_pose();

    const double pose_hash = get_mat_hash(current_camera_pose);
    if (pose_hash == current_pose_hash_) {
        current_pose_hash_ = pose_hash;
        return "";
    }
    current_pose_hash_ = pose_hash;

    return serialize_as_protobuf(keyframes, all_landmarks, local_landmarks, current_camera_pose);
}

std::string data_serializer::serialize_latest_frame(const unsigned int image_quality) {
    const auto image = frame_publisher_->draw_frame();
    std::vector<uchar> buf;
    const std::vector<int> params{static_cast<int>(cv::IMWRITE_JPEG_QUALITY), static_cast<int>(image_quality)};
    cv::imencode(".jpg", image, buf, params);
    const auto char_buf = reinterpret_cast<const unsigned char*>(buf.data());
    const std::string base64_serial = base64_encode(char_buf, buf.size());
    return base64_serial;
}

#if defined(PATCH_RESEND_LOOP_KEYFRAMES)
stella_vslam::Mat44_t data_serializer::get_keyframe_pose_map(const std::shared_ptr<stella_vslam::data::keyframe> keyfrm,
                                                             std::unordered_map<unsigned int, stella_vslam::Mat44_t>& keyframe_pose_map) {
    stella_vslam::Mat44_t pose;

    const auto id = keyfrm->id_;
    if (keyframe_pose_map.count(id) != 0) {
        pose = keyframe_pose_map.at(id);
    } else {
        pose = keyfrm->get_pose_cw();
        keyframe_pose_map[id] = pose;
    }

    return pose;
}
#endif

std::string data_serializer::serialize_as_protobuf(const std::vector<std::shared_ptr<stella_vslam::data::keyframe>>& keyfrms,
                                                   const std::vector<std::shared_ptr<stella_vslam::data::landmark>>& all_landmarks,
                                                   const std::set<std::shared_ptr<stella_vslam::data::landmark>>& local_landmarks,
                                                   const stella_vslam::Mat44_t& current_camera_pose) {
    map_segment::map map;
    auto message = map.add_messages();
    message->set_tag("0");
    message->set_txt("only map data");

    std::forward_list<map_segment::map_keyframe*> allocated_keyframes;

#if defined(PATCH_RESEND_LOOP_KEYFRAMES)
    std::unordered_map<unsigned int, stella_vslam::Mat44_t> obtained_keyfrms_pose_map;
    std::unordered_map<unsigned int, unsigned int> detected_loop_keyfrms_map;
#endif

    // 1. keyframe registration

#if defined(PATCH_RESEND_LOOP_KEYFRAMES)
    bool update_keyfrms = false;
#endif

    std::unordered_map<unsigned int, double> next_keyframe_hash_map;
    for (const auto& keyfrm : keyfrms) {
        if (!keyfrm || keyfrm->will_be_erased()) {
            continue;
        }

        const auto id = keyfrm->id_;
#if !defined(PATCH_RESEND_LOOP_KEYFRAMES)
        const auto pose = keyfrm->get_pose_cw();
#else
        const auto pose = get_keyframe_pose_map(keyfrm, obtained_keyfrms_pose_map);
#endif
        const auto pose_hash = get_mat_hash(pose); // get zipped code (likely hash)

        next_keyframe_hash_map[id] = pose_hash;

        // check whether the "point" has already been send.
        // and remove it from "keyframe_zip".
        if (keyframe_hash_map_->count(id) != 0) {
            if (keyframe_hash_map_->at(id) == pose_hash) {
                keyframe_hash_map_->erase(id);
                continue;
            }
            keyframe_hash_map_->erase(id);
        }

#if defined(PATCH_RESEND_LOOP_KEYFRAMES)
        update_keyfrms = true;
#endif

        auto keyfrm_obj = map.add_keyframes();
        keyfrm_obj->set_id(keyfrm->id_);
        map_segment::map_Mat44* pose_obj = new map_segment::map_Mat44();
        for (int i = 0; i < 16; i++) {
            int ir = i / 4;
            int il = i % 4;
            pose_obj->add_pose(pose(ir, il));
        }
        keyfrm_obj->set_allocated_pose(pose_obj);
        allocated_keyframes.push_front(keyfrm_obj);
    }
    // add removed keyframes.
    for (const auto& itr : *keyframe_hash_map_) {
        const auto id = itr.first;

        auto keyfrm_obj = map.add_keyframes();
        keyfrm_obj->set_id(id);
    }

    *keyframe_hash_map_ = next_keyframe_hash_map;

#if defined(PATCH_RESEND_LOOP_KEYFRAMES)
    std::vector<std::shared_ptr<stella_vslam::data::keyframe>> covisibilities;
#endif

    // 2. graph registration
    for (const auto& keyfrm : keyfrms) {
        if (!keyfrm || keyfrm->will_be_erased()) {
            continue;
        }

        const unsigned int keyfrm_id = keyfrm->id_;

#if defined(PATCH_RESEND_LOOP_KEYFRAMES)
        const auto loop_edges = keyfrm->graph_node_->get_loop_edges();
#endif

        // covisibility graph
        const auto covisibilities = keyfrm->graph_node_->get_covisibilities_over_min_num_shared_lms(100);
        if (!covisibilities.empty()) {
            for (const auto& covisibility : covisibilities) {
                if (!covisibility || covisibility->will_be_erased()) {
                    continue;
                }
                if (covisibility->id_ < keyfrm_id) {
                    continue;
                }
                const auto edge_obj = map.add_edges();
                edge_obj->set_id0(keyfrm_id);
                edge_obj->set_id1(covisibility->id_);

#if defined(PATCH_RESEND_LOOP_KEYFRAMES)
                if (loop_edges.empty()) {
                    continue;
                }

                // find a pair of nearest loop keyframes to current camera frame from among 
                // all keyframes composing covisibility graph
                if (update_keyfrms) {
                    continue;
                }
                if (keyfrm_id == close_keyfrm_ids_[0] && covisibility->id_ == close_keyfrm_ids_[1]) {
                    continue;
                }
                if (std::labs((long int)keyfrm_id - (long int)covisibility->id_) == 1)  {
                    if (close_keyfrm_ids_[0] == 0 && close_keyfrm_ids_[1] == 0) {
                        close_keyfrm_ids_[0] = keyfrm_id;
                        close_keyfrm_ids_[1] = covisibility->id_;
                        close_keyfrm_poses_[0] = get_keyframe_pose_map(keyfrm, obtained_keyfrms_pose_map);
                        close_keyfrm_poses_[1] = get_keyframe_pose_map(covisibility, obtained_keyfrms_pose_map);
                    } else {
                        const auto t_cf_kf0_old = (current_camera_pose - close_keyfrm_poses_[0]).block(0, 3, 3, 1);        
                        const auto t_cf_kf1_old = (current_camera_pose - close_keyfrm_poses_[1]).block(0, 3, 3, 1);
                        const auto pose0_new = get_keyframe_pose_map(keyfrm, obtained_keyfrms_pose_map);
                        const auto pose1_new = get_keyframe_pose_map(covisibility, obtained_keyfrms_pose_map);
                        const auto t_cf_kf0_new = (current_camera_pose - pose0_new).block(0, 3, 3, 1);
                        const auto t_cf_kf1_new = (current_camera_pose - pose1_new).block(0, 3, 3, 1);
                        if (((t_cf_kf0_old.cwiseAbs() - t_cf_kf0_new.cwiseAbs()).array() > 0).all() &&
                            ((t_cf_kf1_old.cwiseAbs() - t_cf_kf1_new.cwiseAbs()).array() > 0).all()) {
                            close_keyfrm_ids_[0] = keyfrm_id;
                            close_keyfrm_ids_[1] = covisibility->id_;
                            close_keyfrm_poses_[0] = pose0_new;
                            close_keyfrm_poses_[1] = pose0_new;
                        }
                    }
                }
#endif
            }
        }

        // spanning tree
        auto spanning_parent = keyfrm->graph_node_->get_spanning_parent();
        if (spanning_parent) {
            const auto edge_obj = map.add_edges();
            edge_obj->set_id0(keyfrm_id);
            edge_obj->set_id1(spanning_parent->id_);
        }

        // loop edges
#if !defined(PATCH_RESEND_LOOP_KEYFRAMES)
        const auto loop_edges = keyfrm->graph_node_->get_loop_edges();
#endif
        for (const auto& loop_edge : loop_edges) {
            if (!loop_edge) {
                continue;
            }
            if (loop_edge->id_ < keyfrm_id) {
                continue;
            }
            const auto edge_obj = map.add_edges();
            edge_obj->set_id0(keyfrm_id);
            edge_obj->set_id1(loop_edge->id_);

#if defined(PATCH_RESEND_LOOP_KEYFRAMES)
            if (detected_loop_keyfrms_map.count(keyfrm_id) != 0) {
                if (detected_loop_keyfrms_map.at(keyfrm_id) != loop_edge->id_) {
                    detected_loop_keyfrms_map.erase(keyfrm_id);
                    detected_loop_keyfrms_map[loop_edge->id_] = keyfrm_id;
                }
            } else {
                detected_loop_keyfrms_map[keyfrm_id] = loop_edge->id_;
            }
#endif
        }
    }

#if defined(PATCH_RESEND_LOOP_KEYFRAMES)
    // no keyframes to be registred means that current camera frame is running
    // over loop keyframs. we infrom a pair of nearest keyframes to it
    if (!update_keyfrms) {
        if (!detected_loop_keyfrms_map.empty()) {
            if (!(close_keyfrm_ids_[0] == 0 && close_keyfrm_ids_[1] == 0)) {
                auto message = map.add_messages();
                message->set_tag("100");
                message->set_txt("{\"closePairKeyframes\":"
                                    "[" + std::to_string(close_keyfrm_ids_[0]) + ","
                                        + std::to_string(close_keyfrm_ids_[1]) + "]"
                                 "}");
            }

            std::string txt = "";
            for (auto itr = detected_loop_keyfrms_map.begin(); itr != detected_loop_keyfrms_map.end(); ) {
                const auto id0 = itr->first;
                const auto id1 = itr->second;
                txt += "[" + std::to_string(id0) + "," + std::to_string(id1) + "]";
                itr++;
                if (itr != detected_loop_keyfrms_map.end()) {
                    txt += ",";
                }
            }
            auto message = map.add_messages();
            message->set_tag("101");
            message->set_txt("{\"loopEdgeKeyframes\":[" + txt + "]}");
        }
    }
#endif

    // 3. landmark registration

    std::unordered_map<unsigned int, double> next_point_hash_map;
    for (const auto& landmark : all_landmarks) {
        if (!landmark || landmark->will_be_erased()) {
            continue;
        }

        const auto id = landmark->id_;
        const auto pos = landmark->get_pos_in_world();
        const auto zip = get_vec_hash(pos);

        // point exists on next_point_zip.
        next_point_hash_map[id] = zip;

        // remove point from point_zip.
        if (point_hash_map_->count(id) != 0) {
            if (point_hash_map_->at(id) == zip) {
                point_hash_map_->erase(id);
                continue;
            }
            point_hash_map_->erase(id);
        }
        const unsigned int rgb[] = {0, 0, 0};

        // add to protocol buffers
        auto landmark_obj = map.add_landmarks();
        landmark_obj->set_id(id);
        for (int i = 0; i < 3; i++) {
            landmark_obj->add_coords(pos[i]);
        }
        for (int i = 0; i < 3; i++) {
            landmark_obj->add_color(rgb[i]);
        }
    }
    // removed points are remaining in "point_zips".
    for (const auto& itr : *point_hash_map_) {
        const auto id = itr.first;

        auto landmark_obj = map.add_landmarks();
        landmark_obj->set_id(id);
    }
    *point_hash_map_ = next_point_hash_map;

    // 4. local landmark registration

    for (const auto& landmark : local_landmarks) {
        map.add_local_landmarks(landmark->id_);
    }

    // 5. current camera pose registration
    map_segment::map_Mat44 pose_obj{};
    for (int i = 0; i < 16; i++) {
        int ir = i / 4;
        int il = i % 4;
        pose_obj.add_pose(current_camera_pose(ir, il));
    }
    map.set_allocated_current_frame(&pose_obj);

    std::string buffer;
    map.SerializeToString(&buffer);

    for (const auto keyfrm_obj : allocated_keyframes) {
        keyfrm_obj->clear_pose();
    }
    map.release_current_frame();

    const auto* cstr = reinterpret_cast<const unsigned char*>(buffer.c_str());
    return base64_encode(cstr, buffer.length());
}

std::string data_serializer::base64_encode(unsigned char const* bytes_to_encode, unsigned int in_len) {
    static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::stringstream ss;
    int i = 0, j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    while (in_len--) {
        char_array_3[i++] = *(bytes_to_encode++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for (i = 0; (i < 4); i++)
                ss << base64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i > 0) {
        for (j = i; j < 3; j++)
            char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        for (j = 0; (j < i + 1); j++)
            ss << base64_chars[char_array_4[j]];
        while ((i++ < 3))
            ss << '=';
    }
    return ss.str();
}

} // namespace socket_publisher
