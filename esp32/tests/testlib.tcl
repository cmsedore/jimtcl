# ESP32 Jim Tcl Test Framework (pure Tcl)
# Load via: eval [fs read /data/tests/testlib.tcl]
# Fallback when the C-level [test] extension is not compiled in.

set _test(total) 0
set _test(pass)  0
set _test(fail)  0
set _test(skip)  0
array set _test_constraints {esp32 1 jim 1}

proc testConstraint {name args} {
    if {[llength $args] == 1} {
        set ::_test_constraints($name) [lindex $args 0]
    } elseif {[info exists ::_test_constraints($name)]} {
        return $::_test_constraints($name)
    } else {
        return 0
    }
}

proc testReset {} {
    set ::_test(total) 0
    set ::_test(pass)  0
    set ::_test(fail)  0
    set ::_test(skip)  0
}

proc testReport {} {
    puts ""
    puts "--- Test Report ---"
    puts "Total:   $::_test(total)"
    puts "Passed:  $::_test(pass)"
    puts "Failed:  $::_test(fail)"
    puts "Skipped: $::_test(skip)"
    puts "-------------------"
}

proc testSuite {name} {
    puts "\n=== $name ==="
}

proc test {id desc args} {
    set body {}; set expected {}; set match exact
    set constraints {}; set returnCodes {ok return}
    foreach {opt val} $args {
        switch -- $opt {
            -body        { set body $val }
            -result      { set expected $val }
            -match       { set match $val }
            -constraints { set constraints $val }
            -returnCodes { set returnCodes $val }
            default      { error "test $id: unknown option $opt" }
        }
    }

    incr ::_test(total)
    foreach c $constraints {
        if {![testConstraint $c]} {
            incr ::_test(skip)
            puts "SKIP: $id ($desc)"
            return skip
        }
    }
    set rc [catch {uplevel 1 $body} result]
    set code_name [lindex {ok error return break continue signal} $rc]
    if {$code_name eq ""} { set code_name $rc }

    if {$code_name ni $returnCodes && $rc ni $returnCodes} {
        incr ::_test(fail)
        puts "FAIL: $id ($desc)"
        puts "  expected returnCode in {$returnCodes}, got $code_name"
        return fail
    }
    set ok 0
    switch -- $match {
        exact  { set ok [expr {$expected eq $result}] }
        glob   { set ok [string match $expected $result] }
        regexp { set ok [regexp $expected $result] }
        default { error "test $id: unknown match mode $match" }
    }

    if {$ok} {
        incr ::_test(pass)
        puts "PASS: $id ($desc)"
        return pass
    }

    incr ::_test(fail)
    puts "FAIL: $id ($desc)"
    puts "  expected: $expected"
    puts "       got: $result"
    return fail
}
