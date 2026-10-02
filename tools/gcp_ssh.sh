#!/bin/sh
# Run a command on a benchmark VM (or copy files to it) with OpenSSH, using the
# key gcloud registered for the project. Avoids Windows gcloud's PuTTY plink.
#   sh tools/gcp_ssh.sh run  <ip> <command...>
#   sh tools/gcp_ssh.sh put  <ip> <local file>...      (copies into the remote home dir)
#   sh tools/gcp_ssh.sh user <ip>                      (find the login name that works)
KEY=${GCE_KEY:-$HOME/.ssh/gce}
USERNAME=${GCE_USER:-MYHSIEH}
OPTS="-i $KEY -o StrictHostKeyChecking=accept-new -o ConnectTimeout=20 -o BatchMode=yes -o ServerAliveInterval=15"
cmd=$1; ip=$2; shift 2
case "$cmd" in
  run)  ssh $OPTS "$USERNAME@$ip" "$@" ;;
  put)  scp $OPTS "$@" "$USERNAME@$ip:" ;;
  user) for u in MYHSIEH myhsieh; do
          if ssh $OPTS "$u@$ip" true 2>/dev/null; then echo "$u"; exit 0; fi
        done; echo "no login worked"; exit 1 ;;
  *)    echo "usage: gcp_ssh.sh run|put|user <ip> ..."; exit 2 ;;
esac
