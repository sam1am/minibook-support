#!/bin/bash

echo "Testing physical click and drag fix..."
echo "======================================"
echo ""
echo "Stopping moused service..."
sudo systemctl stop moused

echo ""
echo "Starting moused with fix..."
echo "Try to do a PHYSICAL CLICK AND DRAG now!"
echo "The cursor should move while holding the physical button."
echo ""
echo "Press Ctrl+C when done testing to restore the service."
echo ""

# Run the fixed moused
sudo ./moused/bin/moused

# When user presses Ctrl+C, this will run
echo ""
echo "Restarting moused service..."
sudo systemctl start moused
echo "Done!"
