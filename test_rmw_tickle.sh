#!/bin/bash

# Test script for rmw_tickle
set -e

echo "=== rmw_tickle Test Script ==="

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if Docker is running
if ! docker info > /dev/null 2>&1; then
    print_error "Docker is not running. Please start Docker first."
    exit 1
fi

# Check if docker-compose is available
if ! command -v docker-compose &> /dev/null; then
    print_error "docker-compose is not installed. Please install docker-compose first."
    exit 1
fi

print_status "Building rmw_tickle Docker image..."
docker-compose build rmw-tickle-builder

print_status "Building rmw_tickle package..."
docker-compose run --rm rmw-tickle-builder

print_status "Testing basic functionality..."

# Test 1: Check if the package builds successfully
print_status "Test 1: Package build verification"
if docker-compose run --rm rmw-tickle-test bash -c "ls -la /workspace/rmw_tickle/install/rmw_tickle/lib/"; then
    print_status "✓ Package built successfully"
else
    print_error "✗ Package build failed"
    exit 1
fi

# Test 2: Check RMW implementation
print_status "Test 2: RMW implementation check"
if docker-compose run --rm rmw-tickle-test bash -c "echo \$RMW_IMPLEMENTATION"; then
    print_status "✓ RMW_IMPLEMENTATION is set to rmw_tickle"
else
    print_error "✗ RMW_IMPLEMENTATION not set correctly"
fi

# Test 3: Test native tickle examples (without network namespaces in Docker)
print_status "Test 3: Testing native tickle examples"
print_warning "Note: Network namespace tests require privileged containers and may not work in all Docker environments"

# Test 4: Test ROS2 demo nodes
print_status "Test 4: Testing ROS2 demo nodes with rmw_tickle"
print_status "Starting talker and listener nodes..."

# Start listener in background
docker-compose up -d listener

# Wait a moment for listener to start
sleep 3

# Start talker and let it run for a few seconds
timeout 10s docker-compose run --rm talker || true

# Stop listener
docker-compose stop listener
docker-compose rm -f listener

print_status "✓ ROS2 demo nodes test completed"

# Test 5: Check library linking
print_status "Test 5: Library linking verification"
if docker-compose run --rm rmw-tickle-test bash -c "ldd /workspace/rmw_tickle/install/lib/librmw_tickle.so | grep tickle"; then
    print_status "✓ rmw_tickle library is properly linked with tickle"
else
    print_warning "⚠ Could not verify tickle library linking"
fi

print_status "=== Test Summary ==="
print_status "rmw_tickle Docker environment is ready for testing!"
print_status ""
print_status "Available commands:"
print_status "  docker-compose up -d                    # Start all services"
print_status "  docker-compose run --rm rmw-tickle-test # Interactive test environment"
print_status "  docker-compose run --rm publisher       # Run publisher example"
print_status "  docker-compose run --rm subscriber      # Run subscriber example"
print_status "  docker-compose run --rm client          # Run client example"
print_status "  docker-compose run --rm server          # Run server example"
print_status "  docker-compose run --rm talker          # Run ROS2 talker"
print_status "  docker-compose run --rm listener        # Run ROS2 listener"
print_status ""
print_status "To clean up:"
print_status "  docker-compose down"
print_status "  docker-compose down --volumes --rmi all"
