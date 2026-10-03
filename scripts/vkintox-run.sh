#!/bin/sh
# vkintox-run — launch a game with VKIntox enabled.

if [ $# -eq 0 ] || [ "$1" = "--help" ] || [ "$1" = "-h" ]; then
  echo "Usage: vkintox-run <command...>"
  echo ""
  echo "Launch a game with VKIntox enabled."
  echo "Sets ENABLE_VKINTOX=1."
  exit 0
fi

export ENABLE_VKINTOX=1

exec "$@"
