# rmw_tickle Docker Testing Environment

This document explains how to test rmw_tickle in a Docker environment. It is based on ROS2 Jazzy.

## Requirements

- Docker
- docker-compose
- Minimum 4GB RAM (recommended)

## Quick Start

### 1. Automated Test Execution

```bash
./test_rmw_tickle.sh
```

This script performs the following:
- Docker image build
- rmw_tickle package build
- Basic functionality testing
- ROS2 demo node testing

### 2. Manual Testing

#### Build
```bash
# Build Docker image
docker-compose build

# Build rmw_tickle package
docker-compose run --rm rmw-tickle-builder
```

#### Run Test Environment
```bash
# Interactive test environment
docker-compose run --rm rmw-tickle-test

# Or run in background
docker-compose up -d rmw-tickle-test
```

## Available Services

### Basic Services
- `rmw-tickle-builder`: Build-only container
- `rmw-tickle-test`: Interactive test environment

### Example Applications
- `publisher`: UInt64 topic publisher
- `subscriber`: UInt64 topic subscriber
- `client`: SetBool service client
- `server`: SetBool service server

### ROS2 Demo Nodes
- `talker`: ROS2 standard talker node
- `listener`: ROS2 standard listener node

## Test Scenarios

### 1. Basic Functionality Test
```bash
# Test in interactive environment
docker-compose run --rm rmw-tickle-test

# Inside container:
source /workspace/rmw_tickle/install/setup.bash
echo $RMW_IMPLEMENTATION  # Should be rmw_tickle
```

### 2. Native Example Test
```bash
# Publisher/Subscriber test
docker-compose run --rm publisher &
docker-compose run --rm subscriber

# Service client/server test
docker-compose run --rm server &
docker-compose run --rm client
```

### 3. ROS2 Demo Node Test
```bash
# Talker/Listener test
docker-compose run --rm listener &
docker-compose run --rm talker
```

### 4. Network Namespace Test (Limited)
```bash
# Attempt to create namespace inside container
docker-compose run --rm rmw-tickle-test bash -c "
    make createns
    make runpublisher &
    make runsubscriber
"
```

## Environment Variables

- `RMW_IMPLEMENTATION=rmw_tickle`: RMW implementation setting
- `ROS_DOMAIN_ID=0`: ROS2 domain ID

## Troubleshooting

### Build Failure
```bash
# Rebuild without cache
docker-compose build --no-cache
```

### Permission Issues
```bash
# Privileged mode required for network namespace testing
docker-compose run --rm --privileged rmw-tickle-test
```

### Memory Issues
```bash
# Check Docker memory limits
docker system df
docker system prune  # Clean up unnecessary images/containers
```

## Cleanup

```bash
# Stop and remove all containers
docker-compose down

# Complete cleanup including volumes and images
docker-compose down --volumes --rmi all

# Docker system cleanup
docker system prune -a
```

## Development Tips

1. **Fast Iterative Development**: After source code changes, rebuild with `docker-compose run --rm rmw-tickle-builder`
2. **Debugging**: Access interactive environment with `docker-compose run --rm rmw-tickle-test bash`
3. **Log Checking**: Check service logs with `docker-compose logs [service-name]`
4. **Volume Mount**: Source code is automatically mounted, so changes are reflected immediately

## Known Limitations

1. **Network Namespace**: Network namespace functionality may be limited in Docker environment
2. **Permissions**: Some tests may require `--privileged` mode
3. **Performance**: Performance degradation possible due to Docker overhead
