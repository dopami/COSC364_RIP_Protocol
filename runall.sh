#!/bin/bash

read -p "How many RIP sessions you want to create? (1-7): " session_number

echo "Starting $session_number RIP sessions"

for ((i = 1; i <= session_number; i++));
do
echo "$i"
gnome-terminal -- ./rip $i.cfg
done

echo "Enjoy"