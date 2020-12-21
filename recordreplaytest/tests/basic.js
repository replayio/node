
function foo() {
  for (let i = 0; i < 3; i++) {
    bar(i);
  }
}

function bar(i) {
  console.log("HELLO", i);
}

setTimeout(foo, 0);
