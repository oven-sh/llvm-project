; REQUIRES: x86

;; Several objects that weakly *reference* the same symbol each carry their own
;; default for it (an absolute-zero symbol named .weak.<sym>.default.<other>).
;; Those defaults are equivalent and must not be reported as duplicate symbols,
;; whether or not a definition of the symbol is linked in, and whether the
;; second reference comes from a separate translation unit or from ThinLTO
;; importing the referencing function into another module.

; RUN: rm -rf %t.dir && split-file %s %t.dir
; RUN: llc -filetype=obj %t.dir/a.ll -o %t.a.obj
; RUN: llc -filetype=obj %t.dir/b.ll -o %t.b.obj
; RUN: llc -filetype=obj %t.dir/main.ll -o %t.main.obj
; RUN: llc -filetype=obj %t.dir/hook.ll -o %t.hook.obj

;; Native objects, symbol left undefined: both references resolve to null.
; RUN: lld-link /entry:main /subsystem:console %t.main.obj %t.a.obj %t.b.obj /out:%t.undef.exe
;; Native objects, symbol defined by a later object: the definition wins.
; RUN: lld-link /entry:main /subsystem:console %t.main.obj %t.a.obj %t.b.obj %t.hook.obj /out:%t.def.exe /lldmap:%t.def.map
; RUN: FileCheck --check-prefix=DEF %s < %t.def.map
; DEF: hook.obj
; DEF-NOT: .weak.hook

;; ThinLTO: a() and b() are imported into main's module, so the weak reference
;; ends up in several native objects produced by the LTO backend.
; RUN: opt -module-summary %t.dir/a.ll -o %t.a.bc
; RUN: opt -module-summary %t.dir/b.ll -o %t.b.bc
; RUN: opt -module-summary %t.dir/main.ll -o %t.main.bc
; RUN: opt -module-summary %t.dir/hook.ll -o %t.hook.bc
; RUN: lld-link /entry:main /subsystem:console %t.main.bc %t.a.bc %t.b.bc /out:%t.lto-undef.exe
; RUN: lld-link /entry:main /subsystem:console %t.main.bc %t.a.bc %t.b.bc %t.hook.bc /out:%t.lto-def.exe

;; A weak reference and a weak *definition* with a real body still conflict
;; outside of MinGW mode (unchanged behavior).
; RUN: llc -filetype=obj %t.dir/weakdef.ll -o %t.weakdef.obj
; RUN: not lld-link /entry:main /subsystem:console %t.main.obj %t.a.obj %t.weakdef.obj /out:%t.err.exe 2>&1 | FileCheck --check-prefix=ERR %s
; ERR: error: duplicate symbol: hook

;--- a.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

declare extern_weak void @hook()

define dso_local i32 @a() {
entry:
  %cmp = icmp ne ptr @hook, null
  br i1 %cmp, label %if.then, label %if.end
if.then:
  tail call void @hook()
  br label %if.end
if.end:
  %r = phi i32 [ 1, %if.then ], [ 0, %entry ]
  ret i32 %r
}

;--- b.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

declare extern_weak void @hook()

define dso_local i32 @b() {
entry:
  %cmp = icmp ne ptr @hook, null
  br i1 %cmp, label %if.then, label %if.end
if.then:
  tail call void @hook()
  br label %if.end
if.end:
  %r = phi i32 [ 2, %if.then ], [ 0, %entry ]
  ret i32 %r
}

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

declare dso_local i32 @a()
declare dso_local i32 @b()

define dso_local i32 @main() {
entry:
  %x = tail call i32 @a()
  %y = tail call i32 @b()
  %s = add i32 %x, %y
  ret i32 %s
}

;--- hook.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define dso_local void @hook() {
entry:
  ret void
}

;--- weakdef.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define weak dso_local void @hook() {
entry:
  ret void
}
