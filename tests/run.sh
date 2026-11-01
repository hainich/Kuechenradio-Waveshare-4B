#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
cc -std=c11 -Wall -Wextra -Werror -Imain main/url_codec.c tests/test_url_codec.c -o /tmp/kuechenradio-url-codec-test
/tmp/kuechenradio-url-codec-test
printf 'URL parser tests: OK\n'
cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_audio_ring.c -o /tmp/kuechenradio-audio-ring-test
/tmp/kuechenradio-audio-ring-test
sh tests/run_stream.sh
