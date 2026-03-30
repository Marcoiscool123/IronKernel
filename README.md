#IRONKERNEL SETUP#
DOWNLOAD REQUIRED PACKAGES FOR SETTING UP IRONKERNEL:
NASM, GCC, BIN UTILS, GRUB-MKRESCUE, QEMU-X86_64, XORRISO, PULSE-AUDIO, MTOOLS, and FFMPEG(oh wait and get make :)
WARNING:IF YOU DONT HAVE THESE EITHER IT WONT COMPILE OR SOME TOOLS WONT WORK.

#INSTALLING PACKAGES ON LINUX..

#ARCH LINUX
sudo pacman -S nasm gcc binutils grub xorriso qemu-system-x86

#DEBIAN BASED DISTROS or WSL
sudo apt install nasm gcc binutils grub-pc-bin xorriso qemu-system-x86

#FEDORA
sudo dnf install nasm gcc binutils grub2-tools xorriso qemu-system-x86

#Verify your tools to se if they work!
nasm --version        # NASM 2.x
  gcc --version         # GCC 10+ recommended
  ld --version          # GNU ld 2.35+
  grub-mkrescue --version
  qemu-system-x86_64 --version

#STEP 2 Downloading Ironkernel Folder
Download it from here and enter it:
cd /path/to/ironkernel

#STEP 3 Building:
First Clean out the Build folder for any junk:
make clean
Then Build Ironkernel:
make or make all

#STEP 4 Run test without Disk32.img
 SDL_VIDEO_HIGHDPI_DISABLED=1 qemu-system-x86_64 \
      -cpu Skylake-Client \
      -cdrom myos.iso \
      -boot order=d \
      -m 256M \
      -vga std \
      -display sdl,gl=off \
      -nic user,model=e1000 \
      -usb -device usb-tablet \
      -machine pc,pcspk-audiodev=snd0 \
      -audiodev pa,id=snd0 \
      -device AC97,audiodev=snd0 \
      -serial file:/tmp/ik_serial.log \
      -debugcon file:/tmp/ik_debug.log \
      -D /tmp/ik_qemu.log -d guest_errors,cpu_reset,int \
      -monitor unix:/tmp/ik_monitor.sock,server,nowait

  No audio? No promblem:
  SDL_VIDEO_HIGHDPI_DISABLED=1 qemu-system-x86_64 \
      -cpu Skylake-Client \
      -cdrom myos.iso \
      -boot order=d \
      -m 256M \
      -vga std \
      -display sdl,gl=off \
      -nic user,model=e1000 \
      -usb -device usb-tablet \
      -serial file:/tmp/ik_serial.log \
      -debugcon file:/tmp/ik_debug.log
#STEP 5 No disk.img? thats fine:
        Kernel boots: Yes
        VBE Graphics: Yes
        Keyboard + Mouse: Yes
        Shell: Most Commands
        Memory Managment: YES
        e1000 Networking: YES
        Running ELF programs: No disk to read from
        FAT32 File system: NO
    Install Mtools using your Package Manager.
    Then create a 256MB Raw disk img:
    dd if=/dev/zero of=disk32.img bs=1M count=256
    Format it:
     DISK32.IMG How mkfs.fat -F 32 -n "IRONKERNEL" disk32.img

  #GET PROGRAMS:
  make disk
  then make run to try everything out..


  MAKE CHEAT SHEET
  make all          — build kernel ISO from source          
  make run          — boot in QEMU (requires disk32.img)                                                                                    
  make debug        — boot paused, GDB on :1234
  make verify       — validate multiboot2 header                                                                                            
  make clean        — remove build artifacts                
  make disk         — copy ELF programs into disk32.img                                                                                     
  make import-wav WAV=/path/to/file.wav  — install startup sound


  HAVE FUN WITH IRONKERNEL
    

        
