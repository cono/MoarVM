use v6;

class Spesh is rw {
    has Str $.before;
    has Str $.after;
    has Str $.name;
    has Str $.cuid;
    has Str $.diff;
}

enum Target <Before After>;

sub supersmartmatch($thing) {
    given $thing {
        when Pair {
            say "making a supersmartmatcher for $thing.key() -> $thing.value().perl()";
            return -> $to_match { $to_match."$thing.key()"() ~~ supersmartmatch($thing.value) }
        }
        when Str {
            return -> $to_match { $to_match ~~ / $($thing) / }
        }
        when Junction {
            return -> $to_match { $to_match ~~ supersmartmatch($thing) }
        }
        default {
            return $thing
        }
    }
}

sub MAIN($filename?, :$matcher?) {
    my %speshes;
    my Spesh  $current;
    my Target $target;

    my $linecount;

    if $filename {
        $*ARGFILES = open($filename, :r);
    } else {
        $*ARGFILES = $*IN;
    }

    my $ssm = do if $matcher {
        supersmartmatch EVAL $matcher
    } else {
        True
    }

    for lines() {
        my $line = $_;
        when /^ 'Specialized ' \' $<name>=[<-[\']>*] \' ' (cuid: ' $<cuid>=[<-[\)]>+] ')'/ {
            if $current and %speshes{$current.cuid}:exists {
                $current.cuid ~= "_";
            }
            %speshes{$current.cuid} = $current if $current;
            $current .= new(name => $<name>.Str, cuid => $<cuid>.Str, diff => "");
        }
        when /^ 'Before:'/ {
            $target = Before;
        }
        when /^ 'After:'/ {
            $target = After;
        }
        when /^'  ' (.*)/ {
            if $target ~~ Before {
                $current.before ~= $line ~ "\n";
            } elsif $target ~~ After {
                $current.after ~= $line ~ "\n";
            }
        }
        $linecount++;
    }

    say "we've parsed $linecount lines";
    say "we have the following cuids:";

    mkdir "spesh_diffs_before";
    mkdir "spesh_diffs_after";

    my @results;
    my @interesting;

    for %speshes.values {
        spurt "spesh_diffs_before/{.cuid}.txt", "{.name} (before)\n{.before}";
        spurt "spesh_diffs_after/{.cuid}.txt", "{.name} (after)\n{.after}";

        @results.push: $_.diff = qq:x"git diff --patience --color=always --no-index spesh_diffs_before/{.cuid}.txt spesh_diffs_after/{.cuid}.txt";
        @interesting.push: $_.diff if $_ ~~ $ssm;

        printf "%30s %s (%s)\n", .cuid, ($_ ~~ $ssm ?? "*" !! " "), .name;
    }

    for %speshes.values {
    }

    for @interesting || @results {
        .say
    }
    if $matcher and 1 < @interesting < @results {
        note "matcher selected {+@interesting} out of {+@results} cuids";
    }
    if $matcher and 0 == @interesting {
        note "matcher matched no cuids"
    }
}