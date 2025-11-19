#!/bin/bash

# Interactive Jitter Configuration Test Script for moused
# Allows users to test different jitter and jump threshold values
# and optionally install the modified version

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
JITTER_THRESHOLD=3
JUMP_THRESHOLD=50
NEED_TO_RESTART_SERVICE=false

# Paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MOUSED_DIR="$SCRIPT_DIR/moused"
MOUSED_SRC="$MOUSED_DIR/src/moused.c"
MOUSED_BACKUP="$MOUSED_SRC.backup"
MOUSED_BIN="$MOUSED_DIR/bin/moused"

# Function to display usage
usage() {
    cat << EOF
${BLUE}Interactive Jitter Configuration Test Script${NC}

Usage: $0 [OPTIONS]

Options:
    --jitter-threshold N    Set jitter threshold (default: 3 pixels)
    --jump-threshold N      Set jump threshold (default: 50 pixels)
    --jitter N              Short form for --jitter-threshold
    --jump N                Short form for --jump-threshold
    -h, --help              Show this help message

Description:
    This script allows you to test different jitter suppression settings
    for the moused trackpad driver. It will:

    1. Temporarily modify the threshold values in moused.c
    2. Build a test version of moused
    3. Stop the system service and run the test version
    4. Let you test the trackpad behavior
    5. Optionally install the version with your preferred settings

Examples:
    # Test with default values (jitter=3, jump=50)
    $0

    # Test with custom values
    $0 --jitter-threshold 5 --jump-threshold 100

    # Using short form
    $0 --jitter 2 --jump 30

Current defaults:
    Jitter threshold: ${GREEN}3 pixels${NC} (movements smaller than this are ignored)
    Jump threshold:   ${GREEN}50 pixels${NC} (sudden jumps larger than this are suppressed)

EOF
    exit 0
}

# Function to print colored messages
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Function to clean up on exit
cleanup() {
    if [ -f "$MOUSED_BACKUP" ]; then
        print_info "Restoring original moused.c..."
        mv "$MOUSED_BACKUP" "$MOUSED_SRC"
        print_success "Original file restored"
    fi

    # Clean up build artifacts
    if [ -d "$MOUSED_DIR" ]; then
        cd "$MOUSED_DIR"
        make clean > /dev/null 2>&1 || true
    fi
}

# Function to restore system service
restore_service() {
    if [ "$NEED_TO_RESTART_SERVICE" = "true" ]; then
        if systemctl is-active --quiet moused.service 2>/dev/null; then
            print_info "System service is already running"
        else
            print_info "Restarting system moused service..."
            sudo systemctl start moused.service 2>/dev/null || print_warning "Could not restart system service (it may not be installed)"
        fi
    fi
}

# Parse command-line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --jitter-threshold|--jitter)
            JITTER_THRESHOLD="$2"
            shift 2
            ;;
        --jump-threshold|--jump)
            JUMP_THRESHOLD="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            print_error "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Validate numeric inputs
if ! [[ "$JITTER_THRESHOLD" =~ ^[0-9]+$ ]]; then
    print_error "Jitter threshold must be a positive integer"
    exit 1
fi

if ! [[ "$JUMP_THRESHOLD" =~ ^[0-9]+$ ]]; then
    print_error "Jump threshold must be a positive integer"
    exit 1
fi

# Validate paths
if [ ! -f "$MOUSED_SRC" ]; then
    print_error "Could not find moused.c at: $MOUSED_SRC"
    exit 1
fi

# Set up cleanup trap (will be modified during testing)
trap 'cleanup; restore_service' EXIT

# Display configuration
echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}  Jitter Configuration Test${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "Jitter threshold: ${GREEN}${JITTER_THRESHOLD}${NC} pixels"
echo -e "Jump threshold:   ${GREEN}${JUMP_THRESHOLD}${NC} pixels"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n"

print_info "These settings will be tested (not installed yet)"
echo ""

# Backup original file
print_info "Backing up original moused.c..."
cp "$MOUSED_SRC" "$MOUSED_BACKUP"
print_success "Backup created"

# Modify the #define values
print_info "Modifying threshold values in moused.c..."
sed -i "s/^#define JITTER_THRESHOLD.*/#define JITTER_THRESHOLD ${JITTER_THRESHOLD}      \/\/ Ignore movements smaller than this (pixels)/" "$MOUSED_SRC"
sed -i "s/^#define JUMP_THRESHOLD.*/#define JUMP_THRESHOLD ${JUMP_THRESHOLD}       \/\/ Suppress jumps larger than this (likely lift noise)/" "$MOUSED_SRC"
print_success "Thresholds updated"

# Build the test version
print_info "Building test version of moused..."
cd "$MOUSED_DIR"
if make clean > /dev/null 2>&1 && make > /dev/null 2>&1; then
    print_success "Build completed successfully"
else
    print_error "Build failed"
    exit 1
fi

# Check if moused service is running and stop it
NEED_TO_RESTART_SERVICE=false
if systemctl is-active --quiet moused.service 2>/dev/null; then
    NEED_TO_RESTART_SERVICE=true
    print_warning "System moused service is running. It needs to be stopped for testing."
    print_info "Stopping system moused service..."
    sudo systemctl stop moused.service
    print_success "Service stopped"
fi

# Run the test version
echo ""
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}  Starting test version of moused${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "Settings:"
echo -e "  • Jitter threshold: ${GREEN}${JITTER_THRESHOLD}${NC} pixels"
echo -e "  • Jump threshold:   ${GREEN}${JUMP_THRESHOLD}${NC} pixels"
echo ""
echo -e "${YELLOW}Test your trackpad now!${NC}"
echo -e "Try moving the cursor slowly and quickly to feel the difference."
echo ""
echo -e "Press ${GREEN}Enter${NC} when you're done testing to proceed to the menu"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""

# Run moused in the background
sudo "$MOUSED_BIN" -d &
MOUSED_PID=$!

# Wait for user to press Enter
read -r

# Stop the test moused instance
print_info "Stopping test instance..."
sudo kill $MOUSED_PID 2>/dev/null || true
sleep 1
print_success "Test instance stopped"

# After testing, ask user what to do
echo ""
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}  Test Complete${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo "What would you like to do?"
echo ""
echo "  1) Install these settings permanently"
echo "  2) Discard and restore original settings"
echo "  3) Try different values"
echo ""
read -p "Enter your choice (1/2/3): " -n 1 -r
echo ""
echo ""

case $REPLY in
    1)
        print_info "Installing moused with new settings..."
        cd "$MOUSED_DIR"
        if sudo make install; then
            print_success "Installation complete!"
            echo ""
            echo -e "${GREEN}Settings installed:${NC}"
            echo -e "  • Jitter threshold: ${GREEN}${JITTER_THRESHOLD}${NC} pixels"
            echo -e "  • Jump threshold:   ${GREEN}${JUMP_THRESHOLD}${NC} pixels"
            echo ""

            # Remove backup since we're keeping the changes
            rm -f "$MOUSED_BACKUP"

            # Restart service if it was running before
            if [ "$NEED_TO_RESTART_SERVICE" = "true" ]; then
                print_info "Restarting moused service..."
                sudo systemctl restart moused.service
                print_success "Service restarted with new settings"
            fi

            # Don't restore service on exit since we already restarted it
            NEED_TO_RESTART_SERVICE=false
        else
            print_error "Installation failed"
            restore_service
            exit 1
        fi
        ;;
    2)
        print_info "Discarding changes and restoring original settings..."
        mv "$MOUSED_BACKUP" "$MOUSED_SRC"
        print_success "Original settings restored"
        restore_service
        ;;
    3)
        print_info "Restoring original settings..."
        mv "$MOUSED_BACKUP" "$MOUSED_SRC"
        restore_service
        echo ""
        print_info "Run the script again with different values:"
        echo -e "  ${GREEN}$0 --jitter <value> --jump <value>${NC}"
        ;;
    *)
        print_warning "Invalid choice. Restoring original settings..."
        mv "$MOUSED_BACKUP" "$MOUSED_SRC"
        restore_service
        ;;
esac

echo ""
print_success "Done!"
