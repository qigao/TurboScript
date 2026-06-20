/* Test IO module functions */

print("=== Testing IO Module Functions ===");

/* Test 1: now() */
print("\n1. Testing now():");
let ts = now();
print("  now() = " + ts);
print("  (should be > 1000000000)");

/* Test 2: date_diff() */
print("\n2. Testing date_diff():");
let diff = date_diff(1000, 500);
print("  date_diff(1000, 500) = " + diff);
print("  (should be 500)");

/* Test 3: date_add() */
print("\n3. Testing date_add():");
let base = 1000000;
let future = date_add(base, 1);
print("  date_add(" + base + ", 1) = " + future);
print("  (should be " + (base + 86400) + ")");

/* Test 4: date_components() */
print("\n4. Testing date_components():");
let parts = date_components(now());
print("  date_components(now()) = " + parts);
print("  length = " + parts.length);

/* Test 5: date_from_parts() */
print("\n5. Testing date_from_parts():");
let ts2 = date_from_parts(2024, 1, 1);
print("  date_from_parts(2024, 1, 1) = " + ts2);

/* Test 6: weekday() */
print("\n6. Testing weekday():");
let wd = weekday(now());
print("  weekday(now()) = " + wd);
print("  (should be 0-6)");

/* Test 7: year_day() */
print("\n7. Testing year_day():");
let yd = year_day(now());
print("  year_day(now()) = " + yd);
print("  (should be 1-366)");

/* Test 8: listdir() */
print("\n8. Testing listdir():");
let files = listdir(".");
print("  listdir(\".\") type: " + typeof(files));
if (typeof(files) == "list") {
    print("  count: " + files.length);
    if (files.length > 0) {
        print("  first entry: " + files[0]);
    }
} else {
    print("  ERROR: listdir() did not return a list!");
    print("  returned: " + files);
}

print("\n=== Test Complete ===");
