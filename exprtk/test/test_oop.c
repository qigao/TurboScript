/**
 * @file test_oop.c
 * @brief Unit tests for TurboScript object-oriented runtime support.
 */

#include "exprtk.h"
#include "exprtk_class.h"
#include "tinytest.h"
#include <math.h>
#include <string.h>

#define TEST_TOLERANCE 1e-6

static double value_to_double(exprtk_value_t value) {
    if (value.type == EXPRTK_VAL_INTEGER) return (double)value.data.integer;
    if (value.type == EXPRTK_VAL_NUMBER) return value.data.number;
    return 0.0;
}

static exprtk_value_t eval_script(const char *code) {
    exprtk_env_t env;
    exprtk_env_init(&env);

    exprtk_node_t *root = exprtk_parse(code, strlen(code));
    check_not_null(root);

    exprtk_value_t result = exprtk_eval(root, &env);
    check_int_eq(env.flow, exprtk_FLOW_NORMAL);
    check_int_eq(env.aborted, 0);

    exprtk_free(root);
    exprtk_env_free(&env);
    return result;
}

spec("OOP runtime") {
    describe("classes and instances") {
        it("should initialize fields through constructor and call instance methods") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  constructor(start) { this.value = start; }"
                "  increment() { this.value = this.value + 1; return this.value; }"
                "  get() { return this.value; }"
                "};"
                "c = Counter(10);"
                "c.increment();"
                "c.get()");

            check_double_eq(value_to_double(result), 11.0, TEST_TOLERANCE);
        }

        it("should keep instance fields isolated") {
            exprtk_value_t result = eval_script(
                "class Box {"
                "  constructor(value) { this.value = value; }"
                "  set(value) { this.value = value; }"
                "  get() { return this.value; }"
                "};"
                "a = Box(1);"
                "b = Box(2);"
                "a.set(10);"
                "a.get() + b.get()");

            check_double_eq(value_to_double(result), 12.0, TEST_TOLERANCE);
        }

        it("should ignore explicit constructor return values") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  constructor(value) { this.value = value; return 999; }"
                "  get() { return this.value; }"
                "};"
                "c = Counter(42);"
                "is_instance(c) * 100 + c.get()");

            check_double_eq(value_to_double(result), 142.0, TEST_TOLERANCE);
        }

        it("should keep instance field slots stable across updates") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            const char *script = "class Box {"
                                 "  constructor(value) { this.value = value; }"
                                 "  set(value) { this.value = value; }"
                                 "};"
                                 "box = Box(1);";
            exprtk_node_t *root = exprtk_parse(script, strlen(script));
            check_not_null(root);

            exprtk_eval(root, &env);
            exprtk_value_t box = exprtk_env_get(&env, "box");
            check_int_eq(box.type, EXPRTK_VAL_INSTANCE);

            exprtk_value_t *slot =
                exprtk_instance_get_field_slot(box.data.instance_val.instance, "value");
            check_not_null(slot);
            check_double_eq(value_to_double(*slot), 1.0, TEST_TOLERANCE);

            exprtk_instance_set_field(box.data.instance_val.instance, "other",
                                      (exprtk_value_t){ EXPRTK_VAL_NUMBER, {2.0} });
            exprtk_instance_set_field(box.data.instance_val.instance, "value",
                                      (exprtk_value_t){ EXPRTK_VAL_NUMBER, {42.0} });

            exprtk_value_t *updated_slot =
                exprtk_instance_get_field_slot(box.data.instance_val.instance, "value");
            check_ptr_eq(updated_slot, slot);
            check_double_eq(value_to_double(*updated_slot), 42.0, TEST_TOLERANCE);

            exprtk_value_t field;
            check(exprtk_instance_get_field(box.data.instance_val.instance, "value", &field));
            check_double_eq(value_to_double(field), 42.0, TEST_TOLERANCE);

            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should import known parent field slots into child layouts") {
            exprtk_env_t env;
            exprtk_env_init(&env);

            const char *script = "class Base {"
                                 "  constructor(value) { this.base = value; }"
                                 "};"
                                 "seed = Base(1);"
                                 "class Child extends Base {"
                                 "  constructor(value) { this.extra = value + 1; super(value); }"
                                 "};"
                                 "child = Child(20);";
            exprtk_node_t *root = exprtk_parse(script, strlen(script));
            check_not_null(root);

            exprtk_eval(root, &env);
            exprtk_value_t child = exprtk_env_get(&env, "child");
            check_int_eq(child.type, EXPRTK_VAL_INSTANCE);

            exprtk_instance_t *instance = child.data.instance_val.instance;
            exprtk_class_t *klass = instance->klass;
            check(klass->instance_field_count >= 2);
            if (klass->instance_field_count >= 2) {
                check_str_eq(klass->instance_field_names[0], "base");
                check_str_eq(klass->instance_field_names[1], "extra");
            }

            exprtk_value_t *base_slot = exprtk_instance_get_field_slot(instance, "base");
            exprtk_value_t *extra_slot = exprtk_instance_get_field_slot(instance, "extra");
            check_not_null(base_slot);
            check_not_null(extra_slot);
            check_double_eq(value_to_double(*base_slot), 20.0, TEST_TOLERANCE);
            check_double_eq(value_to_double(*extra_slot), 21.0, TEST_TOLERANCE);

            exprtk_instance_set_field(instance, "extra",
                                      (exprtk_value_t){ EXPRTK_VAL_NUMBER, {2.0} });
            exprtk_instance_set_field(instance, "base",
                                      (exprtk_value_t){ EXPRTK_VAL_NUMBER, {40.0} });
            check_ptr_eq(exprtk_instance_get_field_slot(instance, "base"), base_slot);
            check_ptr_eq(exprtk_instance_get_field_slot(instance, "extra"), extra_slot);

            exprtk_free(root);
            exprtk_env_free(&env);
        }

        it("should support explicit new expressions") {
            exprtk_value_t result = eval_script(
                "class Point {"
                "  constructor(x, y) { this.x = x; this.y = y; }"
                "  sum() { return this.x + this.y; }"
                "};"
                "p = new Point(3, 4);"
                "p.sum()");

            check_double_eq(value_to_double(result), 7.0, TEST_TOLERANCE);
        }

        it("should instantiate class values stored in variables") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  constructor(value) { this.value = value; }"
                "  get() { return this.value; }"
                "};"
                "CounterAlias = Counter;"
                "c = CounterAlias(42);"
                "c.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should instantiate class values with new") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  constructor(value) { this.value = value; }"
                "  get() { return this.value; }"
                "};"
                "CounterAlias = Counter;"
                "c = new CounterAlias(42);"
                "c.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should let class value variables shadow class names when called") {
            exprtk_value_t result = eval_script(
                "class Counter { get() { return 1; } };"
                "class OtherCounter { get() { return 42; } };"
                "Counter = OtherCounter;"
                "c = Counter();"
                "c.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should let class value variables shadow class names with new") {
            exprtk_value_t result = eval_script(
                "class Counter { get() { return 1; } };"
                "class OtherCounter { get() { return 42; } };"
                "Counter = OtherCounter;"
                "c = new Counter();"
                "c.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should let class value variables shadow class names with instanceof") {
            exprtk_value_t result = eval_script(
                "class Counter {};"
                "class OtherCounter {};"
                "Counter = OtherCounter;"
                "c = OtherCounter();"
                "c instanceof Counter");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject class calls after the class variable is overwritten") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Counter { get() { return 1; } };"
                "  var Counter = 2;"
                "  Counter();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject new expressions after the class variable is overwritten") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Counter { get() { return 1; } };"
                "  var Counter = 2;"
                "  new Counter();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should treat overwritten class variables as non-classes for instanceof") {
            exprtk_value_t result = eval_script(
                "class Counter {};"
                "c = Counter();"
                "var Counter = 2;"
                "c instanceof Counter");

            check_double_eq(value_to_double(result), 0.0, TEST_TOLERANCE);
        }

        it("should call bound methods stored in variables") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  constructor(value) { this.value = value; }"
                "  add(delta) { return this.value + delta; }"
                "};"
                "c = Counter(40);"
                "add = c.add;"
                "add(2)");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should keep instances returned from functions alive") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  constructor(value) { this.value = value; }"
                "  increment() { this.value = this.value + 1; return this.value; }"
                "  get() { return this.value; }"
                "};"
                "func make_counter(value) { return Counter(value); };"
                "c = make_counter(7);"
                "c.increment();"
                "c.get()");

            check_double_eq(value_to_double(result), 8.0, TEST_TOLERANCE);
        }

        it("should keep bound methods returned from functions alive") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  constructor(value) { this.value = value; }"
                "  add(delta) { return this.value + delta; }"
                "};"
                "func make_add(value) { c = Counter(value); return c.add; };"
                "add = make_add(40);"
                "add(2)");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should keep instances of local classes returned from functions alive") {
            exprtk_value_t result = eval_script(
                "func make_counter(value) {"
                "  class Counter {"
                "    constructor(value) { this.value = value; }"
                "    get() { return this.value; }"
                "  };"
                "  return Counter(value);"
                "};"
                "c = make_counter(42);"
                "c.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should keep bound methods of local classes returned from functions alive") {
            exprtk_value_t result = eval_script(
                "func make_add(value) {"
                "  class Counter {"
                "    constructor(value) { this.value = value; }"
                "    add(delta) { return this.value + delta; }"
                "  };"
                "  c = Counter(value);"
                "  return c.add;"
                "};"
                "add = make_add(40);"
                "add(2)");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should dispatch overloaded instance methods by arity") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  value() { return 1; }"
                "  value(x) { return x + 1; }"
                "  value(x, y) { return x + y + 1; }"
                "};"
                "c = Counter();"
                "c.value() + c.value(10) + c.value(10, 20)");

            check_double_eq(value_to_double(result), 43.0, TEST_TOLERANCE);
        }

        it("should dispatch overloaded instance methods by argument type") {
            exprtk_value_t result = eval_script(
                "class Token {};"
                "class Counter {"
                "  value(x) { return 0; }"
                "  value(x: number) { return 1; }"
                "  value(x: string) { return 2; }"
                "  value(x: list) { return 4; }"
                "  value(x: map) { return 8; }"
                "  value(x: class) { return 16; }"
                "  value(x: function) { return 32; }"
                "  value(x: null) { return 64; }"
                "};"
                "c = Counter();"
                "fn = () => 1;"
                "c.value(41) + "
                "c.value(\"x\") + "
                "c.value(list(1)) + "
                "c.value(map {x: 1}) + "
                "c.value(Token) + "
                "c.value(fn) + "
                "c.value(null)");

            check_double_eq(value_to_double(result), 127.0, TEST_TOLERANCE);
        }

        it("should dispatch overloaded instance methods by class type") {
            exprtk_value_t result = eval_script(
                "class Shape {};"
                "class Circle extends Shape {};"
                "class Square extends Shape {};"
                "class Picker {"
                "  value(x) { return 1; }"
                "  value(x: Shape) { return 20; }"
                "  value(x: Circle) { return 400; }"
                "};"
                "p = Picker();"
                "c = Circle();"
                "s = Square();"
                "p.value(c) * 100 + p.value(s) * 10 + p.value(null)");

            check_double_eq(value_to_double(result), 40201.0, TEST_TOLERANCE);
        }

        it("should spread list values in instance method calls") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  value(a, b, c) { return (a == \"x\") * 100 + b.code * 10 + c; }"
                "};"
                "args = list(\"x\", map {code: 4}, 2);"
                "Counter().value(...args)");

            check_double_eq(value_to_double(result), 142.0, TEST_TOLERANCE);
        }

        it("should dispatch overloaded constructors by arity") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  constructor() { this.value = 1; }"
                "  constructor(value) { this.value = value; }"
                "  get() { return this.value; }"
                "};"
                "Counter().get() * 100 + Counter(42).get()");

            check_double_eq(value_to_double(result), 142.0, TEST_TOLERANCE);
        }

        it("should dispatch overloaded constructors by argument type") {
            exprtk_value_t result = eval_script(
                "class Token {};"
                "class Box {"
                "  constructor(value) { this.value = 0; }"
                "  constructor(value: number) { this.value = 1; }"
                "  constructor(value: string) { this.value = 2; }"
                "  constructor(value: list) { this.value = 4; }"
                "  constructor(value: map) { this.value = 8; }"
                "  constructor(value: class) { this.value = 16; }"
                "  constructor(value: function) { this.value = 32; }"
                "  constructor(value: null) { this.value = 64; }"
                "  get() { return this.value; }"
                "};"
                "fn = () => 1;"
                "Box(41).get() + "
                "Box(\"x\").get() + "
                "Box(list(1)).get() + "
                "Box(map {x: 1}).get() + "
                "Box(Token).get() + "
                "Box(fn).get() + "
                "Box(null).get()");

            check_double_eq(value_to_double(result), 127.0, TEST_TOLERANCE);
        }

        it("should spread list values in constructor calls") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  constructor(a, b, c) { this.value = (a == \"x\") * 100 + b.code * 10 + c; }"
                "  get() { return this.value; }"
                "};"
                "args = list(\"x\", map {code: 4}, 2);"
                "Counter(...args).get()");

            check_double_eq(value_to_double(result), 142.0, TEST_TOLERANCE);
        }

        it("should keep class values returned from functions alive") {
            exprtk_value_t result = eval_script(
                "func make_counter_class() {"
                "  class Counter {"
                "    constructor(value) { this.value = value; }"
                "    get() { return this.value; }"
                "  };"
                "  return Counter;"
                "};"
                "CounterClass = make_counter_class();"
                "c = CounterClass(42);"
                "c.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should let constructors capture enclosing variables") {
            exprtk_value_t result = eval_script(
                "func make_counter(value) {"
                "  offset = 2;"
                "  class Counter {"
                "    constructor(start) { this.value = start + offset; }"
                "    get() { return this.value; }"
                "  };"
                "  return Counter(value);"
                "};"
                "c = make_counter(40);"
                "c.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should let instance methods capture enclosing variables") {
            exprtk_value_t result = eval_script(
                "func make_counter(value) {"
                "  offset = 2;"
                "  class Counter {"
                "    constructor(value) { this.value = value; }"
                "    get() { return this.value + offset; }"
                "  };"
                "  return Counter(value);"
                "};"
                "c = make_counter(40);"
                "c.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }
    }

    describe("static methods") {
        it("should call static methods on class values") {
            exprtk_value_t result = eval_script(
                "class Math2 {"
                "  static add(a, b) { return a + b; }"
                "};"
                "Math2.add(3, 5)");

            check_double_eq(value_to_double(result), 8.0, TEST_TOLERANCE);
        }

        it("should inherit static methods from parent classes") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  static value() { return 40; }"
                "};"
                "class Child extends Base {};"
                "Child.value() + 2");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should call parent static methods through super") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  static value(x) { return x + 1; }"
                "};"
                "class Child extends Base {"
                "  static value(x) { return super.value(x) * 2; }"
                "};"
                "Child.value(20)");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should dispatch overloaded parent static methods through super by type") {
            exprtk_value_t result = eval_script(
                "class Token {};"
                "class Base {"
                "  static value(x) { return 0; }"
                "  static value(x: number) { return 1; }"
                "  static value(x: string) { return 2; }"
                "  static value(x: list) { return 4; }"
                "  static value(x: map) { return 8; }"
                "  static value(x: class) { return 16; }"
                "  static value(x: function) { return 32; }"
                "  static value(x: null) { return 64; }"
                "};"
                "class Child extends Base {"
                "  static value(x) { return super.value(x); }"
                "};"
                "fn = () => 1;"
                "Child.value(41) + "
                "Child.value(\"x\") + "
                "Child.value(list(1)) + "
                "Child.value(map {x: 1}) + "
                "Child.value(Token) + "
                "Child.value(fn) + "
                "Child.value(null)");

            check_double_eq(value_to_double(result), 127.0, TEST_TOLERANCE);
        }

        it("should call static methods stored in variables") {
            exprtk_value_t result = eval_script(
                "class Math2 {"
                "  static add(a, b) { return a + b; }"
                "};"
                "add = Math2.add;"
                "add(20, 22)");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should preserve super context for stored static methods") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  static value(x) { return x + 1; }"
                "};"
                "class Child extends Base {"
                "  static value(x) { return super.value(x) * 2; }"
                "};"
                "value = Child.value;"
                "value(20)");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should reject this inside static methods") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Counter { static value() { return this; } };"
                "  Counter.value();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject captured this inside static methods") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Maker {"
                "    make() { class Counter { static value() { return this; } }; return Counter; }"
                "  };"
                "  CounterClass = Maker().make();"
                "  CounterClass.value();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should let static methods capture enclosing variables") {
            exprtk_value_t result = eval_script(
                "func make_counter_class() {"
                "  offset = 2;"
                "  class Counter { static value(base) { return base + offset; } };"
                "  return Counter;"
                "};"
                "CounterClass = make_counter_class();"
                "CounterClass.value(40)");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should keep captures when static methods are stored") {
            exprtk_value_t result = eval_script(
                "func make_value() {"
                "  offset = 2;"
                "  class Counter { static value(base) { return base + offset; } };"
                "  return Counter.value;"
                "};"
                "value = make_value();"
                "value(40)");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should dispatch overloaded static methods by arity") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  static value() { return 1; }"
                "  static value(x) { return x + 1; }"
                "  static value(x, y) { return x + y + 1; }"
                "};"
                "Counter.value() + Counter.value(10) + Counter.value(10, 20)");

            check_double_eq(value_to_double(result), 43.0, TEST_TOLERANCE);
        }

        it("should dispatch overloaded static methods by primitive type") {
            exprtk_value_t result = eval_script(
                "class Token {};"
                "class Picker {"
                "  static value(x) { return 0; }"
                "  static value(x: number) { return 1; }"
                "  static value(x: string) { return 2; }"
                "  static value(x: list) { return 4; }"
                "  static value(x: map) { return 8; }"
                "  static value(x: class) { return 16; }"
                "  static value(x: function) { return 32; }"
                "  static value(x: null) { return 64; }"
                "};"
                "fn = () => 1;"
                "Picker.value(41) + "
                "Picker.value(\"x\") + "
                "Picker.value(list(1)) + "
                "Picker.value(map {x: 1}) + "
                "Picker.value(Token) + "
                "Picker.value(fn) + "
                "Picker.value(null)");

            check_double_eq(value_to_double(result), 127.0, TEST_TOLERANCE);
        }

        it("should dispatch overloaded static methods by class type") {
            exprtk_value_t result = eval_script(
                "class Shape {};"
                "class Circle extends Shape {};"
                "class Square extends Shape {};"
                "class Picker {"
                "  static value(x) { return 1; }"
                "  static value(x: Shape) { return 20; }"
                "  static value(x: Circle) { return 400; }"
                "};"
                "Picker.value(Circle()) * 100 + Picker.value(Square()) * 10 + Picker.value(null)");

            check_double_eq(value_to_double(result), 40201.0, TEST_TOLERANCE);
        }
    }

    describe("static fields") {
        it("should read fields assigned on class values") {
            exprtk_value_t result = eval_script(
                "class Store {};"
                "Store.value = 40;"
                "Store.value + 2");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should inherit static fields from parent classes") {
            exprtk_value_t result = eval_script(
                "class Base {};"
                "Base.value = 40;"
                "class Child extends Base {};"
                "Child.value + 2");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should read parent static fields through super") {
            exprtk_value_t result = eval_script(
                "class Base {};"
                "Base.value = 40;"
                "class Child extends Base {"
                "  static value() { return super.value + 2; }"
                "};"
                "Child.value()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should assign parent static fields through super") {
            exprtk_value_t result = eval_script(
                "class Base {};"
                "Base.value = 1;"
                "class Child extends Base {"
                "  static set(value) { super.value = value; }"
                "};"
                "Child.set(42);"
                "Base.value");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should let child static fields shadow parent fields") {
            exprtk_value_t result = eval_script(
                "class Base {};"
                "Base.value = 1;"
                "class Child extends Base {};"
                "Child.value = 41;"
                "Child.value + Base.value");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should share static fields through class aliases") {
            exprtk_value_t result = eval_script(
                "class Store {};"
                "Alias = Store;"
                "Alias.value = 42;"
                "Store.value");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should keep static fields on class values returned from functions") {
            exprtk_value_t result = eval_script(
                "func make_store_class() {"
                "  class Store {};"
                "  Store.value = 42;"
                "  return Store;"
                "};"
                "StoreClass = make_store_class();"
                "StoreClass.value");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should keep inherited static fields on returned class values") {
            exprtk_value_t result = eval_script(
                "func make_child_class() {"
                "  class Base {};"
                "  Base.value = 40;"
                "  class Child extends Base {};"
                "  return Child;"
                "};"
                "ChildClass = make_child_class();"
                "ChildClass.value + 2");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should reject missing parent static fields read through super") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Base {};"
                "  class Child extends Base { static value() { return super.missing; } };"
                "  Child.value();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }
    }

    describe("private members") {
        it("should allow methods to access private fields through this") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  constructor(value) { this._value = value; }"
                "  get() { return this._value; }"
                "};"
                "c = Counter(17);"
                "c.get()");

            check_double_eq(value_to_double(result), 17.0, TEST_TOLERANCE);
        }

        it("should allow methods to call private methods through this") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  constructor(value) { this._value = value; }"
                "  _add(delta) { return this._value + delta; }"
                "  add2() { return this._add(2); }"
                "};"
                "c = Counter(40);"
                "c.add2()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should reject external private field access") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Counter { constructor(value) { this._value = value; } };"
                "  c = Counter(1);"
                "  c._value;"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject external private method calls") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Counter { _value() { return 1; } };"
                "  c = Counter();"
                "  c._value();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should allow static methods to access private static fields") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  static init(value) { Counter._value = value; }"
                "  static get() { return Counter._value; }"
                "};"
                "Counter.init(42);"
                "Counter.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should allow static methods to call private static methods") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  static _value() { return 42; }"
                "  static value() { return Counter._value(); }"
                "};"
                "Counter.value()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should reject external private static field access") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Counter { static init() { Counter._value = 1; } };"
                "  Counter.init();"
                "  Counter._value;"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject external private static field assignment") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Counter {};"
                "  Counter._value = 1;"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject non-variable member assignment receivers") {
            exprtk_value_t result = eval_script(
                "side = 0;"
                "try {"
                "  class Box { constructor(value) { this.value = value; } };"
                "  class Maker { make() { return Box(1); } };"
                "  Maker().make().value = (side = 1);"
                "  99"
                "} catch (e) { side }");

            check_double_eq(value_to_double(result), 0.0, TEST_TOLERANCE);
        }

        it("should reject external private static method calls") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Counter { static _value() { return 1; } };"
                "  Counter._value();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should allow private methods inside the declaring class") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  private value() { return 42; }"
                "  get() { return this.value(); }"
                "};"
                "Counter().get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should dispatch private overloaded methods by keyword type inside the declaring class") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  private value(x: map) { return 40; }"
                "  private value(x: null) { return 2; }"
                "  get() { return this.value(map {x: 1}) + this.value(null); }"
                "};"
                "Counter().get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should allow declared private instance fields inside the declaring class") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  private value = 40;"
                "  add(delta) { this.value = this.value + delta; return this.value; }"
                "};"
                "Counter().add(2)");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should reject external declared private instance fields") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Counter { private value = 1; };"
                "  Counter().value;"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should allow protected instance fields from subclasses") {
            exprtk_value_t result = eval_script(
                "class Base { protected value = 40; };"
                "class Child extends Base { add() { this.value = this.value + 2; return this.value; } };"
                "Child().add()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should reject external protected instance fields") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Base { protected value = 1; };"
                "  Base().value;"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject external private methods declared with keywords") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Counter { private value() { return 1; } };"
                "  Counter().value();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject external private overloaded methods selected by keyword type") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Counter { private value(x: map) { return 1; } };"
                "  Counter().value(map {x: 1});"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should allow protected methods from subclasses") {
            exprtk_value_t result = eval_script(
                "class Base { protected value() { return 40; } };"
                "class Child extends Base { get() { return super.value() + 2; } };"
                "Child().get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should dispatch protected overloaded methods by keyword type from subclasses") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  protected value(x: map) { return 40; }"
                "  protected value(x: null) { return 1; }"
                "};"
                "class Child extends Base {"
                "  get() { return super.value(map {x: 1}) + super.value(null) + 1; }"
                "};"
                "Child().get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should reject external protected methods") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Base { protected value() { return 1; } };"
                "  Base().value();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should read declared fields without initializers as null") {
            exprtk_value_t result = eval_script(
                "class Box {"
                "  value;"
                "};"
                "b = Box();"
                "is_null(b.value) * 40 + ((b.value == null) * 2)");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should preserve declared private fields on returned class values") {
            exprtk_value_t result = eval_script(
                "func make_counter() {"
                "  class Counter {"
                "    private value = 40;"
                "    get() { return this.value + 2; }"
                "  };"
                "  return Counter;"
                "};"
                "CounterAlias = make_counter();"
                "CounterAlias().get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should allow private static methods inside the declaring class") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  private static value() { return 40; }"
                "  static get() { return Counter.value() + 2; }"
                "};"
                "Counter.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should dispatch private static overloaded methods by keyword type inside the declaring class") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  private static value(x: map) { return 40; }"
                "  private static value(x: null) { return 2; }"
                "  static get() { return Counter.value(map {x: 1}) + Counter.value(null); }"
                "};"
                "Counter.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should allow declared private static fields inside the declaring class") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  private static value = 40;"
                "  static add() { Counter.value = Counter.value + 2; return Counter.value; }"
                "};"
                "Counter.add()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should reject external declared private static fields") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Counter { private static value = 1; };"
                "  Counter.value;"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject external private static methods declared with keywords") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Counter { private static value() { return 1; } };"
                "  Counter.value();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject external private static overloaded methods selected by keyword type") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Counter { private static value(x: map) { return 1; } };"
                "  Counter.value(map {x: 1});"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should allow protected static methods from subclasses") {
            exprtk_value_t result = eval_script(
                "class Base { protected static value() { return 40; } };"
                "class Child extends Base { static get() { return super.value() + 2; } };"
                "Child.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should dispatch protected static overloaded methods by keyword type from subclasses") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  protected static value(x: map) { return 40; }"
                "  protected static value(x: null) { return 1; }"
                "};"
                "class Child extends Base {"
                "  static get() { return super.value(map {x: 1}) + super.value(null) + 1; }"
                "};"
                "Child.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should reject external protected static methods") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Base { protected static value() { return 1; } };"
                "  Base.value();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject external protected static overloaded methods selected by keyword type") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Base { protected static value(x: map) { return 1; } };"
                "  Base.value(map {x: 1});"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }
    }

    describe("type utilities") {
        it("should report class, instance, and bound method types") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  constructor(value) { this.value = value; }"
                "  get() { return this.value; }"
                "};"
                "c = Counter(5);"
                "(typeof(Counter) == \"class\") * 100 + "
                "(typeof(c) == \"instance\") * 10 + "
                "(typeof(c.get) == \"function\")");

            check_double_eq(value_to_double(result), 111.0, TEST_TOLERANCE);
        }

        it("should expose OOP values through predicates") {
            exprtk_value_t result = eval_script(
                "class Counter {"
                "  get() { return 1; }"
                "};"
                "c = Counter();"
                "is_class(Counter) * 100 + is_instance(c) * 10 + is_function(c.get)");

            check_double_eq(value_to_double(result), 111.0, TEST_TOLERANCE);
        }

        it("should expose OOP values through predicates on map members") {
            exprtk_value_t result = eval_script(
                "func make_bundle() {"
                "  class Counter { get() { return 1; } };"
                "  return map{ Counter: Counter, make: func() { return Counter(); } };"
                "};"
                "bundle = make_bundle();"
                "is_class(bundle.Counter) * 100 + "
                "is_function(bundle.make) * 10 + "
                "(typeof(bundle.Counter) == \"class\")");

            check_double_eq(value_to_double(result), 111.0, TEST_TOLERANCE);
        }
    }

    describe("inheritance") {
        it("should find inherited instance methods") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  get() { return this.value; }"
                "};"
                "class Child extends Base {"
                "  constructor(value) { this.value = value; }"
                "};"
                "c = Child(42);"
                "c.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should call parent constructors with super and support instanceof") {
            exprtk_value_t result = eval_script(
                "class Animal {"
                "  constructor(name) { this.name = name; }"
                "  name_length() { return this.name.length; }"
                "};"
                "class Dog extends Animal {"
                "  constructor(name, weight) { super(name); this.weight = weight; }"
                "  score() { return this.name_length() + this.weight; }"
                "};"
                "d = Dog(\"Rex\", 7);"
                "d.score() + (d instanceof Dog) * 10 + (d instanceof Animal) * 100");

            check_double_eq(value_to_double(result), 120.0, TEST_TOLERANCE);
        }

        it("should inherit parent constructors by default") {
            exprtk_value_t result = eval_script(
                "class Animal {"
                "  constructor(name, weight) { this.name = name; this.weight = weight; }"
                "  score() { return this.name.length + this.weight; }"
                "};"
                "class Dog extends Animal {};"
                "d = Dog(\"Rex\", 39);"
                "d.score()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should let super call inherited default constructors") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  constructor(value) { this.value = value; }"
                "};"
                "class Middle extends Base {};"
                "class Child extends Middle {"
                "  constructor(value) { super(value + 1); }"
                "  get() { return this.value; }"
                "};"
                "Child(41).get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should support class value aliases with instanceof") {
            exprtk_value_t result = eval_script(
                "class Animal {"
                "  constructor(name) { this.name = name; }"
                "};"
                "class Dog extends Animal {};"
                "AnimalAlias = Animal;"
                "DogAlias = Dog;"
                "d = DogAlias(\"Rex\");"
                "(d instanceof DogAlias) * 10 + (d instanceof AnimalAlias)");

            check_double_eq(value_to_double(result), 11.0, TEST_TOLERANCE);
        }

        it("should support class value aliases in extends clauses") {
            exprtk_value_t result = eval_script(
                "class Base { value() { return 42; } };"
                "Parent = Base;"
                "class Child extends Parent {};"
                "Child().value()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should let class value variables shadow parent class names in extends clauses") {
            exprtk_value_t result = eval_script(
                "class Base { value() { return 1; } };"
                "class OtherBase { value() { return 42; } };"
                "Base = OtherBase;"
                "class Child extends Base {};"
                "Child().value()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should reject overwritten class variables in extends clauses") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Base { value() { return 1; } };"
                "  var Base = 2;"
                "  class Child extends Base {};"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should allow methods marked override when a parent method matches") {
            exprtk_value_t result = eval_script(
                "class Base { value() { return 40; } };"
                "class Child extends Base { override value() { return super.value() + 2; } };"
                "Child().value()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should allow typed override methods when a parent signature matches") {
            exprtk_value_t result = eval_script(
                "class Base { value(x: map) { return 40; } value(x: null) { return 1; } };"
                "class Child extends Base {"
                "  override value(x: map) { return super.value(x) + 2; }"
                "};"
                "Child().value(map {x: 1})");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should reject override methods without matching parent methods") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Base {};"
                "  class Child extends Base { override value() { return 1; } };"
                "  Child();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject typed override methods without matching parent signatures") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Base { value(x: null) { return 1; } };"
                "  class Child extends Base { override value(x: map) { return 2; } };"
                "  Child();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject extending final classes") {
            exprtk_value_t result = eval_script(
                "try {"
                "  final class Base {};"
                "  class Child extends Base {};"
                "  Child();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject overriding final methods") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Base { final value() { return 1; } };"
                "  class Child extends Base { value() { return 2; } };"
                "  Child();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject overriding final typed methods with matching signatures") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Base { final value(x: map) { return 1; } value(x: null) { return 2; } };"
                "  class Child extends Base { value(x: map) { return 3; } };"
                "  Child();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should call overridden parent methods with super.method") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  value(x) { return x + 1; }"
                "};"
                "class Child extends Base {"
                "  value(x) { return super.value(x) * 2; }"
                "};"
                "c = Child();"
                "c.value(10)");

            check_double_eq(value_to_double(result), 22.0, TEST_TOLERANCE);
        }

        it("should dispatch overloaded parent methods through super by arity") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  value() { return 1; }"
                "  value(x) { return x + 1; }"
                "  value(x, y) { return x + y + 1; }"
                "};"
                "class Child extends Base {"
                "  value(x) { return super.value() + super.value(x) + super.value(x, 20); }"
                "};"
                "Child().value(10)");

            check_double_eq(value_to_double(result), 43.0, TEST_TOLERANCE);
        }

        it("should dispatch overloaded parent methods through super by type") {
            exprtk_value_t result = eval_script(
                "class Token {};"
                "class Base {"
                "  value(x) { return 0; }"
                "  value(x: number) { return 1; }"
                "  value(x: string) { return 2; }"
                "  value(x: list) { return 4; }"
                "  value(x: map) { return 8; }"
                "  value(x: class) { return 16; }"
                "  value(x: function) { return 32; }"
                "  value(x: null) { return 64; }"
                "};"
                "class Child extends Base {"
                "  value(x) { return super.value(x); }"
                "};"
                "c = Child();"
                "fn = () => 1;"
                "c.value(41) + "
                "c.value(\"x\") + "
                "c.value(list(1)) + "
                "c.value(map {x: 1}) + "
                "c.value(Token) + "
                "c.value(fn) + "
                "c.value(null)");

            check_double_eq(value_to_double(result), 127.0, TEST_TOLERANCE);
        }

        it("should spread list values in super method calls") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  value(a, b, c) { return (a == \"x\") * 100 + b.code * 10 + c; }"
                "};"
                "class Child extends Base {"
                "  value(args) { return super.value(...args); }"
                "};"
                "Child().value(list(\"x\", map {code: 4}, 2))");

            check_double_eq(value_to_double(result), 142.0, TEST_TOLERANCE);
        }

        it("should dispatch overloaded parent constructors through super") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  constructor() { this.value = 1; }"
                "  constructor(value) { this.value = value; }"
                "};"
                "class Child extends Base {"
                "  constructor(value) { super(value); }"
                "  get() { return this.value; }"
                "};"
                "Child(42).get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should dispatch overloaded parent constructors through super by type") {
            exprtk_value_t result = eval_script(
                "class Token {};"
                "class Base {"
                "  constructor(value) { this.value = 0; }"
                "  constructor(value: number) { this.value = 1; }"
                "  constructor(value: string) { this.value = 2; }"
                "  constructor(value: list) { this.value = 4; }"
                "  constructor(value: map) { this.value = 8; }"
                "  constructor(value: class) { this.value = 16; }"
                "  constructor(value: function) { this.value = 32; }"
                "  constructor(value: null) { this.value = 64; }"
                "};"
                "class Child extends Base {"
                "  constructor(value) { super(value); }"
                "  get() { return this.value; }"
                "};"
                "fn = () => 1;"
                "Child(41).get() + "
                "Child(\"x\").get() + "
                "Child(list(1)).get() + "
                "Child(map {x: 1}).get() + "
                "Child(Token).get() + "
                "Child(fn).get() + "
                "Child(null).get()");

            check_double_eq(value_to_double(result), 127.0, TEST_TOLERANCE);
        }

        it("should read inherited instance fields through super") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  constructor(value) { this.value = value; }"
                "};"
                "class Child extends Base {"
                "  constructor(value) { super(value); this.extra = 2; }"
                "  get() { return super.value + this.extra; }"
                "};"
                "Child(40).get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should assign inherited instance fields through super") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  constructor(value) { this.value = value; }"
                "  get() { return this.value; }"
                "};"
                "class Child extends Base {"
                "  set(value) { super.value = value; }"
                "};"
                "c = Child(1);"
                "c.set(42);"
                "c.get()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should resolve super from the inherited method owner class") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  value() { return 1; }"
                "};"
                "class Middle extends Base {"
                "  value() { return super.value() + 1; }"
                "};"
                "class Child extends Middle {};"
                "c = Child();"
                "c.value()");

            check_double_eq(value_to_double(result), 2.0, TEST_TOLERANCE);
        }

        it("should chain super constructors through owner classes") {
            exprtk_value_t result = eval_script(
                "class Base {"
                "  constructor(value) { this.value = value; }"
                "};"
                "class Middle extends Base {"
                "  constructor(value) { super(value + 1); }"
                "};"
                "class Child extends Middle {"
                "  constructor(value) { super(value + 1); }"
                "  get() { return this.value; }"
                "};"
                "c = Child(1);"
                "c.get()");

            check_double_eq(value_to_double(result), 3.0, TEST_TOLERANCE);
        }

        it("should reject classes with unknown parent classes") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Child extends MissingParent {};"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject unknown classes in new expressions") {
            exprtk_value_t result = eval_script(
                "try {"
                "  new MissingClass();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject missing parent methods called through super") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Base {};"
                "  class Child extends Base { value() { return super.missing(); } };"
                "  Child().value();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject super calls when the class has no parent") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Base { value() { return super.value(); } };"
                "  Base().value();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject super constructor calls outside constructors") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class Base { constructor(value) { this.value = value; } };"
                "  class Child extends Base { value() { super(42); return this.value; } };"
                "  Child().value();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }
    }

    describe("invalid contexts") {
        it("should reject this outside instance methods and constructors") {
            exprtk_value_t result = eval_script(
                "try {"
                "  this;"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }
    }

    describe("abstract classes") {
        it("should reject direct abstract class instantiation") {
            exprtk_value_t result = eval_script(
                "try {"
                "  abstract class Shape { abstract area(); };"
                "  Shape();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should allow concrete subclasses to implement abstract methods") {
            exprtk_value_t result = eval_script(
                "abstract class Shape { abstract area(); };"
                "class Square extends Shape {"
                "  constructor(size) { this.size = size; }"
                "  area() { return this.size * this.size; }"
                "};"
                "s = Square(6);"
                "s.area()");

            check_double_eq(value_to_double(result), 36.0, TEST_TOLERANCE);
        }

        it("should match abstract method implementations by arity") {
            exprtk_value_t result = eval_script(
                "abstract class Shape { abstract area(scale); };"
                "class Square extends Shape {"
                "  constructor(size) { this.size = size; }"
                "  area(scale) { return this.size * this.size * scale; }"
                "};"
                "s = Square(6);"
                "s.area(2)");

            check_double_eq(value_to_double(result), 72.0, TEST_TOLERANCE);
        }

        it("should accept keyword typed abstract method implementations") {
            exprtk_value_t result = eval_script(
                "abstract class Handler {"
                "  abstract accept(x: map);"
                "  abstract make(x: null);"
                "};"
                "class Good extends Handler {"
                "  accept(x: map) { return 40; }"
                "  make(x: null) { return 2; }"
                "};"
                "g = Good();"
                "g.accept(map {x: 1}) + g.make(null)");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should reject abstract method implementations with the wrong arity") {
            exprtk_value_t result = eval_script(
                "try {"
                "  abstract class Shape { abstract area(scale); };"
                "  class Broken extends Shape { area() { return 1; } };"
                "  Broken();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject wrong keyword typed abstract method implementations") {
            exprtk_value_t result = eval_script(
                "try {"
                "  abstract class Handler { abstract accept(x: map); };"
                "  class Broken extends Handler { accept(x: null) { return 1; } };"
                "  Broken();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should keep subclasses abstract until inherited abstract methods are implemented") {
            exprtk_value_t result = eval_script(
                "try {"
                "  abstract class Shape { abstract area(); };"
                "  class Broken extends Shape {};"
                "  Broken();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should allow classes to implement interfaces") {
            exprtk_value_t result = eval_script(
                "interface Shape { area(); };"
                "class Square implements Shape {"
                "  constructor(size) { this.size = size; }"
                "  area() { return this.size * this.size; }"
                "};"
                "s = Square(6);"
                "s.area()");

            check_double_eq(value_to_double(result), 36.0, TEST_TOLERANCE);
        }

        it("should allow classes to implement multiple interfaces") {
            exprtk_value_t result = eval_script(
                "interface HasArea { area(); };"
                "interface HasPerimeter { perimeter(); };"
                "class Square implements HasArea, HasPerimeter {"
                "  constructor(size) { this.size = size; }"
                "  area() { return this.size * this.size; }"
                "  perimeter() { return this.size * 4; }"
                "};"
                "s = Square(6);"
                "s.area() + s.perimeter()");

            check_double_eq(value_to_double(result), 60.0, TEST_TOLERANCE);
        }

        it("should allow classes to implement static interface methods") {
            exprtk_value_t result = eval_script(
                "interface Factory { static make(); };"
                "class Counter implements Factory {"
                "  static make() { return 42; }"
                "};"
                "Counter.make()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should allow inherited static methods to satisfy interface requirements") {
            exprtk_value_t result = eval_script(
                "interface Factory { static make(); };"
                "class Base { static make() { return 42; } };"
                "class Child extends Base implements Factory {};"
                "Child.make()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should reject instance methods for static interface requirements") {
            exprtk_value_t result = eval_script(
                "try {"
                "  interface Factory { static make(); };"
                "  class Broken implements Factory { make() { return 1; } };"
                "  Broken();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should reject classes missing interface method implementations") {
            exprtk_value_t result = eval_script(
                "try {"
                "  interface Shape { area(); };"
                "  class Broken implements Shape {};"
                "  Broken();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should allow interfaces to extend interfaces") {
            exprtk_value_t result = eval_script(
                "interface Shape { area(); };"
                "interface Colored { color(); };"
                "interface ColoredShape extends Shape, Colored { label(); };"
                "class Square implements ColoredShape {"
                "  constructor(size) { this.size = size; }"
                "  area() { return this.size * this.size; }"
                "  color() { return 2; }"
                "  label() { return 4; }"
                "};"
                "s = Square(6);"
                "s.area() + s.color() + s.label()");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should reject classes missing inherited interface methods") {
            exprtk_value_t result = eval_script(
                "try {"
                "  interface Shape { area(); };"
                "  interface ColoredShape extends Shape { label(); };"
                "  class Broken implements ColoredShape { label() { return 1; } };"
                "  Broken();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should support instanceof with implemented interfaces") {
            exprtk_value_t result = eval_script(
                "interface Shape { area(); };"
                "interface Colored { color(); };"
                "interface ColoredShape extends Shape, Colored { label(); };"
                "class Square implements ColoredShape {"
                "  area() { return 1; }"
                "  color() { return 2; }"
                "  label() { return 3; }"
                "};"
                "s = Square();"
                "(s instanceof Shape) * 100 + "
                "(s instanceof Colored) * 10 + "
                "(s instanceof ColoredShape)");

            check_double_eq(value_to_double(result), 111.0, TEST_TOLERANCE);
        }

        it("should inherit interface instanceof through class inheritance") {
            exprtk_value_t result = eval_script(
                "interface Shape { area(); };"
                "class Base implements Shape { area() { return 1; } };"
                "class Child extends Base {};"
                "c = Child();"
                "(c instanceof Child) * 100 + "
                "(c instanceof Base) * 10 + "
                "(c instanceof Shape)");

            check_double_eq(value_to_double(result), 111.0, TEST_TOLERANCE);
        }

        it("should allow inherited methods to satisfy interface requirements") {
            exprtk_value_t result = eval_script(
                "interface Shape { area(); };"
                "class Base { area() { return 42; } };"
                "class Child extends Base implements Shape {};"
                "c = Child();"
                "c.area() + (c instanceof Shape)");

            check_double_eq(value_to_double(result), 43.0, TEST_TOLERANCE);
        }

        it("should allow inherited typed methods to satisfy interface requirements") {
            exprtk_value_t result = eval_script(
                "interface Shape { area(); };"
                "interface Handler { handle(x: Shape); };"
                "class Square implements Shape { area() { return 1; } };"
                "class Base { handle(x: Shape) { return 42; } };"
                "class Child extends Base implements Handler {};"
                "Child().handle(Square())");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should dispatch overloaded methods by interface type") {
            exprtk_value_t result = eval_script(
                "interface Shape { area(); };"
                "interface ColoredShape extends Shape { color(); };"
                "class Square implements ColoredShape {"
                "  area() { return 1; }"
                "  color() { return 2; }"
                "};"
                "class Triangle implements Shape { area() { return 3; } };"
                "class Picker {"
                "  value(x) { return 1; }"
                "  value(x: Shape) { return 20; }"
                "  value(x: ColoredShape) { return 400; }"
                "};"
                "p = Picker();"
                "p.value(Square()) * 100 + p.value(Triangle()) * 10 + p.value(null)");

            check_double_eq(value_to_double(result), 40201.0, TEST_TOLERANCE);
        }

        it("should require exact typed interface method implementations") {
            exprtk_value_t result = eval_script(
                "try {"
                "  interface Shape { area(); };"
                "  interface ColoredShape extends Shape { color(); };"
                "  interface Handler { handle(x: Shape); };"
                "  class Broken implements Handler {"
                "    handle(x: ColoredShape) { return 1; }"
                "  };"
                "  Broken();"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should accept exact typed interface method implementations") {
            exprtk_value_t result = eval_script(
                "interface Shape { area(); };"
                "interface Handler { handle(x: Shape); };"
                "class Square implements Shape { area() { return 1; } };"
                "class Good implements Handler {"
                "  handle(x: Shape) { return 42; }"
                "};"
                "Good().handle(Square())");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should accept keyword typed interface method implementations") {
            exprtk_value_t result = eval_script(
                "interface Handler {"
                "  accept_map(x: map);"
                "  accept_class(x: class);"
                "  accept_function(x: function);"
                "  accept_null(x: null);"
                "  static make_map(x: map);"
                "};"
                "class Good implements Handler {"
                "  accept_map(x: map) { return 1; }"
                "  accept_class(x: class) { return 2; }"
                "  accept_function(x: function) { return 4; }"
                "  accept_null(x: null) { return 8; }"
                "  static make_map(x: map) { return 16; }"
                "};"
                "fn = () => 1;"
                "g = Good();"
                "g.accept_map(map {x: 1}) + "
                "g.accept_class(Good) + "
                "g.accept_function(fn) + "
                "g.accept_null(null) + "
                "Good.make_map(map {x: 1})");

            check_double_eq(value_to_double(result), 31.0, TEST_TOLERANCE);
        }

        it("should allow typed implementations for untyped abstract methods") {
            exprtk_value_t result = eval_script(
                "abstract class Base { abstract value(x); };"
                "class Child extends Base { value(x: number) { return x + 1; } };"
                "Child().value(41)");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should preserve interface identity across returned class values") {
            exprtk_value_t result = eval_script(
                "func make_bundle() {"
                "  interface Shape { area(); };"
                "  class Square implements Shape { area() { return 1; } };"
                "  return map{ Shape: Shape, Square: Square };"
                "};"
                "bundle = make_bundle();"
                "ShapeAlias = bundle.Shape;"
                "SquareAlias = bundle.Square;"
                "square = SquareAlias();"
                "square instanceof ShapeAlias");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }

        it("should support instanceof with class value member expressions") {
            exprtk_value_t result = eval_script(
                "func make_bundle() {"
                "  interface Shape { area(); };"
                "  class Square implements Shape { area() { return 1; } };"
                "  return map{ Shape: Shape, Square: Square, nested: map{ Shape: Shape } };"
                "};"
                "bundle = make_bundle();"
                "SquareAlias = bundle.Square;"
                "square = SquareAlias();"
                "(square instanceof bundle.Shape) * 10 + "
                "(square instanceof bundle.nested.Shape)");

            check_double_eq(value_to_double(result), 11.0, TEST_TOLERANCE);
        }

        it("should call callable class and function values through map members") {
            exprtk_value_t result = eval_script(
                "func make_bundle() {"
                "  class Counter {"
                "    constructor(value) { this.value = value; }"
                "    get() { return this.value; }"
                "  };"
                "  return map{ Counter: Counter, add_one: func(x) { return x + 1; } };"
                "};"
                "bundle = make_bundle();"
                "bundle.Counter(41).get() + bundle.add_one(0)");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should preserve interface overload identity across returned class values") {
            exprtk_value_t result = eval_script(
                "func make_bundle() {"
                "  interface Shape { area(); };"
                "  class Square implements Shape { area() { return 1; } };"
                "  class Picker {"
                "    value(x) { return 1; }"
                "    value(x: Shape) { return 42; }"
                "  };"
                "  return map{ Shape: Shape, Square: Square, Picker: Picker };"
                "};"
                "bundle = make_bundle();"
                "SquareAlias = bundle.Square;"
                "PickerAlias = bundle.Picker;"
                "PickerAlias().value(SquareAlias())");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should keep same-name interface overloads scoped by identity") {
            exprtk_value_t result = eval_script(
                "interface Shape { global_area(); };"
                "class Other implements Shape { global_area() { return 1; } };"
                "func make_bundle() {"
                "  interface Shape { local_area(); };"
                "  class Square implements Shape { local_area() { return 1; } };"
                "  class Picker {"
                "    value(x) { return 1; }"
                "    value(x: Shape) { return 42; }"
                "  };"
                "  return map{ Square: Square, Picker: Picker };"
                "};"
                "bundle = make_bundle();"
                "SquareAlias = bundle.Square;"
                "PickerAlias = bundle.Picker;"
                "PickerAlias().value(SquareAlias()) * 100 + PickerAlias().value(Other())");

            check_double_eq(value_to_double(result), 4201.0, TEST_TOLERANCE);
        }

        it("should keep typed interface requirements scoped by identity") {
            exprtk_value_t result = eval_script(
                "interface Shape { global_area(); };"
                "func make_bundle() {"
                "  interface Shape { local_area(); };"
                "  interface Handler { handle(x: Shape); };"
                "  class Square implements Shape { local_area() { return 1; } };"
                "  return map{ Handler: Handler, Shape: Shape, Square: Square };"
                "};"
                "bundle = make_bundle();"
                "HandlerAlias = bundle.Handler;"
                "LocalShape = bundle.Shape;"
                "SquareAlias = bundle.Square;"
                "try {"
                "  class Bad implements HandlerAlias { handle(x: Shape) { return 1; } };"
                "  Bad();"
                "  0"
                "} catch (e) {"
                "  class Good implements HandlerAlias { handle(x: LocalShape) { return 42; } };"
                "  Good().handle(SquareAlias())"
                "}");

            check_double_eq(value_to_double(result), 42.0, TEST_TOLERANCE);
        }

        it("should keep duplicate typed interface requirements by identity") {
            exprtk_value_t result = eval_script(
                "func make_left() {"
                "  interface Shape { left(); };"
                "  interface Handler { handle(x: Shape); };"
                "  class LeftShape implements Shape { left() { return 1; } };"
                "  return map{ Shape: Shape, Handler: Handler, LeftShape: LeftShape };"
                "};"
                "func make_right() {"
                "  interface Shape { right(); };"
                "  interface Handler { handle(x: Shape); };"
                "  class RightShape implements Shape { right() { return 1; } };"
                "  return map{ Shape: Shape, Handler: Handler, RightShape: RightShape };"
                "};"
                "left = make_left();"
                "right = make_right();"
                "LeftHandler = left.Handler;"
                "RightHandler = right.Handler;"
                "LeftShape = left.Shape;"
                "RightShape = right.Shape;"
                "LeftClass = left.LeftShape;"
                "RightClass = right.RightShape;"
                "interface Both extends LeftHandler, RightHandler {};"
                "try {"
                "  class Bad implements Both { handle(x: LeftShape) { return 10; } };"
                "  Bad();"
                "  0"
                "} catch (e) {"
                "  class Good implements Both {"
                "    handle(x: LeftShape) { return 20; }"
                "    handle(x: RightShape) { return 40; }"
                "  };"
                "  Good().handle(LeftClass()) + Good().handle(RightClass())"
                "}");

            check_double_eq(value_to_double(result), 60.0, TEST_TOLERANCE);
        }

        it("should support interface aliases in implements clauses") {
            exprtk_value_t result = eval_script(
                "interface Shape { area(); };"
                "Alias = Shape;"
                "class Square implements Alias { area() { return 42; } };"
                "s = Square();"
                "s.area() + (s instanceof Alias)");

            check_double_eq(value_to_double(result), 43.0, TEST_TOLERANCE);
        }

        it("should support interface aliases in interface extends clauses") {
            exprtk_value_t result = eval_script(
                "interface Shape { area(); };"
                "Alias = Shape;"
                "interface LabeledShape extends Alias { label(); };"
                "class Square implements LabeledShape {"
                "  area() { return 40; }"
                "  label() { return 2; }"
                "};"
                "s = Square();"
                "s.area() + s.label() + (s instanceof Alias)");

            check_double_eq(value_to_double(result), 43.0, TEST_TOLERANCE);
        }

        it("should reject non-interface aliases in implements clauses") {
            exprtk_value_t result = eval_script(
                "try {"
                "  class NotInterface {};"
                "  Alias = NotInterface;"
                "  class Broken implements Alias {};"
                "  0"
                "} catch (e) { typeof(e) == \"string\" }");

            check_double_eq(value_to_double(result), 1.0, TEST_TOLERANCE);
        }
    }
}
