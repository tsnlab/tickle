# Use ROS2 Jazzy as base image
FROM ros:jazzy

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libssl-dev \
    libffi-dev \
    python3-dev \
    python3-pip \
    clang-format \
    clang-tidy \
    iproute2 \
    iputils-ping \
    tcpdump \
    net-tools \
    && rm -rf /var/lib/apt/lists/*

# Install ROS2 development tools
RUN apt-get update && apt-get install -y \
    ros-jazzy-ament-cmake \
    ros-jazzy-ament-lint \
    ros-jazzy-ament-lint-auto \
    ros-jazzy-ament-lint-common \
    ros-jazzy-rmw \
    ros-jazzy-rcutils \
    ros-jazzy-rosidl-runtime-c \
    ros-jazzy-rosidl-typesupport-interface \
    ros-jazzy-rosidl-typesupport-c \
    ros-jazzy-rosidl-typesupport-cpp \
    ros-jazzy-ament-cmake-test \
    ros-jazzy-ament-cmake-lint-cmake \
    ros-jazzy-ament-cmake-copyright \
    ros-jazzy-ament-cmake-cppcheck \
    ros-jazzy-ament-cmake-cpplint \
    ros-jazzy-ament-cmake-flake8 \
    ros-jazzy-ament-cmake-pep257 \
    ros-jazzy-ament-cmake-uncrustify \
    ros-jazzy-ament-cmake-xmllint \
    ros-jazzy-demo-nodes-cpp \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /workspace

# Copy the entire project
COPY . /workspace/

# Create necessary directories and build the tickle library and examples
RUN cd /workspace && mkdir -p obj && make clean && make all

# Build the rmw_tickle package
RUN bash -c "cd /workspace/rmw_tickle && source /opt/ros/jazzy/setup.bash && export LD_LIBRARY_PATH=/workspace:\$LD_LIBRARY_PATH && colcon build --packages-select rmw_tickle"

# Source the workspace
RUN echo "source /workspace/rmw_tickle/install/setup.bash" >> /root/.bashrc

# Set environment variables (RMW_IMPLEMENTATION will be set at runtime)
ENV ROS_DOMAIN_ID=0
ENV LD_LIBRARY_PATH=/workspace:$LD_LIBRARY_PATH

# Create entrypoint script
RUN echo '#!/bin/bash\n\
set -e\n\
\n\
# Source ROS2\n\
source /opt/ros/jazzy/setup.bash\n\
\n\
# Set library path\n\
export LD_LIBRARY_PATH=/workspace:$LD_LIBRARY_PATH\n\
\n\
# Set RMW implementation to tickle at runtime\n\
export RMW_IMPLEMENTATION=rmw_tickle\n\
\n\
# Source the workspace\n\
if [ -f "/workspace/rmw_tickle/install/setup.bash" ]; then\n\
    source /workspace/rmw_tickle/install/setup.bash\n\
fi\n\
\n\
# Execute the command\n\
exec "$@"' > /entrypoint.sh && chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
CMD ["bash"]
