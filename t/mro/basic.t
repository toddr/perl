#!./perl

use strict;
use warnings;

require q(./test.pl); plan(tests => 29);

{
    package MRO_A;
    our @ISA = qw//;
    package MRO_B;
    our @ISA = qw//;
    package MRO_C;
    our @ISA = qw//;
    package MRO_D;
    our @ISA = qw/MRO_A MRO_B MRO_C/;
    package MRO_E;
    our @ISA = qw/MRO_A MRO_B MRO_C/;
    package MRO_F;
    our @ISA = qw/MRO_D MRO_E/;
}

is(mro::get_mro('MRO_F'), 'dfs');
ok(eq_array(
    mro::get_linear_isa('MRO_F'),
    [qw/MRO_F MRO_D MRO_A MRO_B MRO_C MRO_E/]
));
mro::set_mro('MRO_F', 'c3');
is(mro::get_mro('MRO_F'), 'c3');
ok(eq_array(
    mro::get_linear_isa('MRO_F'),
    [qw/MRO_F MRO_D MRO_E MRO_A MRO_B MRO_C/]
));

my @isarev = sort { $a cmp $b } @{mro::get_isarev('MRO_B')};
ok(eq_array(
    \@isarev,
    [qw/MRO_D MRO_E MRO_F/]
));

ok(!mro::is_universal('MRO_B'));

@UNIVERSAL::ISA = qw/MRO_F/;
ok(mro::is_universal('MRO_B'));

@UNIVERSAL::ISA = ();
ok(mro::is_universal('MRO_B'));

# is_universal, get_mro, and get_linear_isa should
# handle non-existant packages sanely
ok(!mro::is_universal('Does_Not_Exist'));
is(mro::get_mro('Also_Does_Not_Exist'), 'dfs');
ok(eq_array(
    mro::get_linear_isa('Does_Not_Exist_Three'),
    [qw/Does_Not_Exist_Three/]
));

# Assigning @ISA via globref
{
    package MRO_TestBase;
    sub testfunc { return 123 }
    package MRO_TestOtherBase;
    sub testfunctwo { return 321 }
    package MRO_M; our @ISA = qw/MRO_TestBase/;
}
*MRO_N::ISA = *MRO_M::ISA;
is(eval { MRO_N->testfunc() }, 123);

# XXX TODO (when there's a way to backtrack through a glob's aliases)
# push(@MRO_M::ISA, 'MRO_TestOtherBase');
# is(eval { MRO_N->testfunctwo() }, 321);

# Simple DESTROY Baseline
{
    my $x = 0;
    my $obj;

    {
        package DESTROY_MRO_Baseline;
        sub new { bless {} => shift }
        sub DESTROY { $x++ }

        package DESTROY_MRO_Baseline_Child;
        our @ISA = qw/DESTROY_MRO_Baseline/;
    }

    $obj = DESTROY_MRO_Baseline->new();
    undef $obj;
    is($x, 1);

    $obj = DESTROY_MRO_Baseline_Child->new();
    undef $obj;
    is($x, 2);
}

# Dynamic DESTROY
{
    my $x = 0;
    my $obj;

    {
        package DESTROY_MRO_Dynamic;
        sub new { bless {} => shift }

        package DESTROY_MRO_Dynamic_Child;
        our @ISA = qw/DESTROY_MRO_Dynamic/;
    }

    $obj = DESTROY_MRO_Dynamic->new();
    undef $obj;
    is($x, 0);

    $obj = DESTROY_MRO_Dynamic_Child->new();
    undef $obj;
    is($x, 0);

    no warnings 'once';
    *DESTROY_MRO_Dynamic::DESTROY = sub { $x++ };

    $obj = DESTROY_MRO_Dynamic->new();
    undef $obj;
    is($x, 1);

    $obj = DESTROY_MRO_Dynamic_Child->new();
    undef $obj;
    is($x, 2);
}

# clearing @ISA in different ways
#  some are destructive to the package, hence the new
#  package name each time
{
    no warnings 'uninitialized';
    {
        package ISACLEAR;
        our @ISA = qw/XX YY ZZ/;
    }
    # baseline
    ok(eq_array(mro::get_linear_isa('ISACLEAR'),[qw/ISACLEAR XX YY ZZ/]));

    # this looks dumb, but it preserves existing behavior for compatibility
    #  (undefined @ISA elements treated as "main")
    $ISACLEAR::ISA[1] = undef;
    ok(eq_array(mro::get_linear_isa('ISACLEAR'),[qw/ISACLEAR XX main ZZ/]));

    # undef the array itself
    undef @ISACLEAR::ISA;
    ok(eq_array(mro::get_linear_isa('ISACLEAR'),[qw/ISACLEAR/]));

    # Now, clear more than one package's @ISA at once
    {
        package ISACLEAR1;
        our @ISA = qw/WW XX/;

        package ISACLEAR2;
        our @ISA = qw/YY ZZ/;
    }
    # baseline
    ok(eq_array(mro::get_linear_isa('ISACLEAR1'),[qw/ISACLEAR1 WW XX/]));
    ok(eq_array(mro::get_linear_isa('ISACLEAR2'),[qw/ISACLEAR2 YY ZZ/]));
    (@ISACLEAR1::ISA, @ISACLEAR2::ISA) = ();

    ok(eq_array(mro::get_linear_isa('ISACLEAR1'),[qw/ISACLEAR1/]));
    ok(eq_array(mro::get_linear_isa('ISACLEAR2'),[qw/ISACLEAR2/]));
}

# Check that recursion bails out "cleanly" in a variety of cases
# (as opposed to say, bombing the interpreter or something)
{
    my @recurse_codes = (
        '@MRO_R1::ISA = "MRO_R2"; @MRO_R2::ISA = "MRO_R1";',
        '@MRO_R3::ISA = "MRO_R4"; push(@MRO_R4::ISA, "MRO_R3");',
        '@MRO_R5::ISA = "MRO_R6"; @MRO_R6::ISA = qw/XX MRO_R5 YY/;',
        '@MRO_R7::ISA = "MRO_R8"; push(@MRO_R8::ISA, qw/XX MRO_R7 YY/)',
    );
    foreach my $code (@recurse_codes) {
        eval $code;
        ok($@ =~ /Recursive inheritance detected/);
    }
}

