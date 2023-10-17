# socket_publisher

This repository contains a customized version of [stella-cv/socket_publisher](https://github.com/stella-cv/socket_publisher).

Original socket_publisher does not re-send loop keyframes to socket_viewer that are published by the SLAM systems.

We wanted all keyframes including loop ones to read out from the SLAM systems, because they are needed to draw completely camera moving locations on a map.

We provides [socket_extractor](https://github.com/ketus-ix/socket_extractor) to extract all keyframe locations from the map information published by the SLAM systems. This customized version of socket_publisher assumes to be used with it instead of original one.

## Installation

### Source code

The source code can be viewed from this [GitHub repository](https://github.com/ketus-ix/socket_publisher).

Cloning the repository:

```bash
git clone -b 0.0.1/resend_loop_keyframes --recursive https://github.com/ketus-ix/socket_publisher.git
```

### Dependencies

Same as original socket_publisher.

See [this stella_vslam document](https://github.com/stella-cv/docs/blob/main/docs/installation.rst#requirements-for-socketviewer).

### Prerequisites

Same as original socket_publisher.

See [this stella_vslam document](https://github.com/stella-cv/docs/blob/main/docs/installation.rst#prerequisites-for-unix).

### Build instructions

After doing the [stella_vslam build procedure](https://github.com/stella-cv/docs/blob/main/docs/installation.rst#build-instructions).

```bash
# When building with support for SocketViewer
cd ~/lib
git clone -b 0.0.1/resend_loop_keyframes --recursive https://github.com/ketus-ix/socket_publisher.git
mkdir -p socket_publisher/build
cd socket_publisher/build
cmake -DPATCH_RESEND_LOOP_KEYFRAMES=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
make -j
sudo make install
```

Adding the cmake option `-DPATCH_RESEND_LOOP_KEYFRAMES=ON` enables re-sending loop keyframes feature in this socket_publisher.

## License

This module was originally included in xdspacelab/openvslam. Therefore, the license follows the original license of xdspacelab/openvslam (BSD 2-Clause).
