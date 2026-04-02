#!/bin/bash

# Configuration menu for IronKernel

# Load the existing configuration
source Config.mk

# Function to display the menu
show_menu() {
    clear
    echo "IronKernel Configuration Menu"
    echo "==========================="
    echo "1) Enable Audio Support: $AUDIO_SUPPORT"
    echo "2) Enable Networking Support: $NETWORKING_SUPPORT"
    echo "3) Enable Graphics Modes: $GRAPHICS_MODE_SUPPORT"
    echo "4) Save and Exit"
    echo "5) Exit without saving"
    echo
    read -p "Select an option [1-5]: " choice
    case $choice in
        1)  
            if [[ "$AUDIO_SUPPORT" == "yes" ]]; then
                AUDIO_SUPPORT="no"
            else
                AUDIO_SUPPORT="yes"
            fi
            ;;
        2)
            if [[ "$NETWORKING_SUPPORT" == "yes" ]]; then
                NETWORKING_SUPPORT="no"
            else
                NETWORKING_SUPPORT="yes"
            fi
            ;;
        3)
            if [[ "$GRAPHICS_MODE_SUPPORT" == "yes" ]]; then
                GRAPHICS_MODE_SUPPORT="no"
            else
                GRAPHICS_MODE_SUPPORT="yes"
            fi
            ;;
        4)
            save_config
            exit 0
            ;;
        5)
            exit 0
            ;;
        *)
            echo "Invalid option!"
            read -n 1 -s -r -p "Press any key to continue..."
            ;;    
    esac
}

# Function to save the configuration
save_config() {
    echo "# IronKernel Configuration" > Config.mk
    echo "AUDIO_SUPPORT=\"$AUDIO_SUPPORT\"" >> Config.mk
    echo "NETWORKING_SUPPORT=\"$NETWORKING_SUPPORT\"" >> Config.mk
    echo "GRAPHICS_MODE_SUPPORT=\"$GRAPHICS_MODE_SUPPORT\"" >> Config.mk
}

# Default settings
AUDIO_SUPPORT="no"
NETWORKING_SUPPORT="no"
GRAPHICS_MODE_SUPPORT="no"

# Main loop
while true; do
    show_menu
}