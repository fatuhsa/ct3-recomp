#!/usr/bin/env bash
set -euo pipefail

# Extracts default.xbe from a Crazy Taxi 3: High Roller (USA) Original Xbox XISO
# and optionally encrypts it to ci/default.xbe.enc for GitHub Actions lifting (XBE_KEY secret).
# Usage:
#   ./tools/extract_and_encrypt_xbe.sh "/path/to/Crazy Taxi 3 - High Roller (USA).iso" [passphrase]

ISO_PATH="${1:-}"
PASSPHRASE="${2:-}"
EXPECTED_SHA1="10096ce907b92a702b52e4ccb3cf15ac167ccb6f"

if [[ -z "$ISO_PATH" || ! -f "$ISO_PATH" ]]; then
  echo "Usage: $0 <path-to-Crazy-Taxi-3-USA.iso> [optional-ci-passphrase]" >&2
  exit 1
fi

mkdir -p assets
perl -e '
use strict; use warnings;
my $iso = $ARGV[0];
open my $fh, "<:raw", $iso or die "Cannot open $iso: $!";
sub read_at { my ($off,$len)=@_; seek($fh,$off,0); my $b=""; read($fh,$b,$len); return $b; }
my $base = 0;
my $hdr = read_at(32*2048, 48);
if (substr($hdr, 0, 20) ne "MICROSOFT*XBOX*MEDIA") {
    $base = 0x18300000;
    $hdr = read_at($base + 32*2048, 48);
    die "Not a valid Xbox XISO image\n" if substr($hdr, 0, 20) ne "MICROSOFT*XBOX*MEDIA";
}
my ($magic, $root_sec, $root_sz) = unpack("a20 V V", $hdr);
my $data = read_at($base + $root_sec*2048, $root_sz);
my @stack=(0); my %seen;
while (@stack) {
    my $off=pop @stack; next if $off==0 && $seen{0}++;
    my ($l,$r,$fsec,$fsz,$attr,$nlen)=unpack("v v V V C C", substr($data,$off*4,14));
    my $name=substr($data,$off*4+14,$nlen);
    push @stack,$r if $r && !$seen{$r}++;
    push @stack,$l if $l && !$seen{$l}++;
    if (!($attr & 0x10) && lc($name) eq "default.xbe") {
        my $xbe = read_at($base + $fsec*2048, $fsz);
        open my $out, ">:raw", "assets/default.xbe" or die $!;
        print $out $xbe;
        close $out;
        print "Extracted assets/default.xbe ($fsz bytes)\n";
        exit 0;
    }
}
die "default.xbe not found in XISO root directory\n";
' "$ISO_PATH"

ACTUAL_SHA1="$(sha1sum assets/default.xbe | awk '{print $1}')"
echo "SHA-1: $ACTUAL_SHA1"
if [[ "$ACTUAL_SHA1" != "$EXPECTED_SHA1" ]]; then
  echo "WARNING: SHA-1 does not match Crazy Taxi 3: High Roller (USA) ($EXPECTED_SHA1)" >&2
fi

if [[ -n "$PASSPHRASE" ]]; then
  mkdir -p ci
  openssl enc -aes-256-cbc -md sha256 -pbkdf2 -iter 200000 -salt \
    -in assets/default.xbe -out ci/default.xbe.enc -pass "pass:${PASSPHRASE}"
  echo "Encrypted ci/default.xbe.enc created. Store the passphrase as GitHub secret XBE_KEY."
fi
