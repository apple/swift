;;===--- sil-mode.el ------------------------------------------------------===;;
;;
;; This source file is part of the Swift.org open source project
;;
;; Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
;; Licensed under Apache License v2.0 with Runtime Library Exception
;;
;; See https://swift.org/LICENSE.txt for license information
;; See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
;;
;;===----------------------------------------------------------------------===;;

(eval-when-compile
  (require 'cl))

;; Create mode-specific tables.
(defvar sil-mode-syntax-table nil
  "Syntax table used while in SIL mode.")

(defvar sil-font-lock-keywords
  (list
   ;; Comments
   '("^#!.*" . font-lock-comment-face)
   ;; Types
   '("\\b[A-Z][a-zA-Z_0-9]*\\b" . font-lock-type-face)
   ;; Floating point constants
   '("\\b[-+]?[0-9]+\.[0-9]+\\b" . font-lock-preprocessor-face)
   ;; Integer literals
   '("\\b[-]?[0-9]+\\b" . font-lock-preprocessor-face)
   ;; Decl and type keywords
   `(,(regexp-opt '("class" "init" "deinit" "extension" "func"
                    "import" "protocol" "static" "struct" "subscript"
                    "typealias" "enum" "var" "let" "where"  "sil_vtable"
                    "sil_global" "private" "public" "internal" "override"
                    "sil_witness_table" "sil_scope")
                  'words) . font-lock-keyword-face)
   ;; SIL Types
   '("\\b[$][*]?[A-Z][z-aA-Z_[0-9]*\\b" . font-lock-type-face)

   ;; SIL Stage
   '("sil_stage" . font-lock-keyword-face)

   ;; SIL Function
   `(,(regexp-opt '("sil" "internal" "thunk")
                  'words) . font-lock-keyword-face)
   ;; SIL Linkage
   `(,(regexp-opt '("public" "hidden" "private" "shared" "public_external"
                    "hidden_external" "shared_external" "private_external")
                  'words) . font-lock-keyword-face)

   ;; SIL Declaration
   `(,(regexp-opt '("getter" "setter" "allocator" "initializer" "enumelt"
                    "destroyer" "globalaccessor" "objc") 'words) .
                    font-lock-keyword-face)

   ;; Highlight attributes written in [...].
   '("\\[\\(.+?\\)\\]" 1 font-lock-keyword-face)

   ;; SIL Instructions - Allocation/Deallocation.
   `(,(regexp-opt '("alloc_stack" "alloc_ref" "alloc_ref_dynamic" "alloc_box"
                    "alloc_global"
                    "dealloc_stack" "dealloc_box" "project_box" "dealloc_ref"
                    "dealloc_partial_ref")
                  'words) . font-lock-keyword-face)

   ;; SIL Instructions - Debug Information.
   `(,(regexp-opt '("debug_value" "debug_value_addr")
                  'words) . font-lock-keyword-face)

   ;; SIL Instructions - Accessing Memory.
   `(,(regexp-opt '("load" "store" "assign"  "mark_uninitialized"
                    "mark_uninitialized_behavior"
                    "mark_function_escape" "copy_addr" "destroy_addr"
                    "index_addr" "index_raw_pointer" "bind_memory" "to")
                  'words) . font-lock-keyword-face)

   ;; SIL Instructions - Borrowing
   `(,(regexp-opt '("load_borrow" "begin_borrow" "store_borrow" "end_borrow") 'words) . font-lock-keyword-face)

   ;; SIL Instructions - Exclusivity
   `(,(regexp-opt '("begin_access" "end_access") 'words) . font-lock-keyword-face)

   ;; SIL Instructions - ownership
   `(,(regexp-opt '("unchecked_ownership_conversion") 'words) . font-lock-keyword-face)

   ;; SIL Instructions - Reference Counting.
   `(,(regexp-opt '("strong_retain"
                    "strong_release" "strong_retain_unowned"
                    "unowned_retain" "unowned_release"
                    "load_weak" "store_weak"
                    "load_unowned" "store_unowned"
                    "fix_lifetime" "mark_dependence"
                    "end_lifetime"
                    "is_unique"
                    "is_escaping_closure"
                    "copy_block"
                    "copy_block_without_escaping"
                    "is_unique")
                  'words) . font-lock-keyword-face)
   ;; Literals
   `(,(regexp-opt '("function_ref"
                    "integer_literal" "float_literal" "string_literal"
                    "global_addr"
                    ) 'words) . font-lock-keyword-face)
   ;; Dynamic Dispatch
   `(,(regexp-opt '("class_method" "super_method" "witness_method"
                    "dynamic_method")
                  'words) . font-lock-keyword-face)
   ;; Function Application
   `(,(regexp-opt '("apply" "partial_apply" "builtin" "try_apply")
                  'words) . font-lock-keyword-face)
   ;; Metatypes
   `(,(regexp-opt '("metatype" "value_metatype"
                    "existential_metatype" "init_existential_metatype"
                    "objc_protocol")
                  'words) . font-lock-keyword-face)
   ;; Aggregate Types
   `(,(regexp-opt '("retain_value" "release_value_addr" "release_value"
                    "release_value_addr" "tuple" "tuple_extract"
                    "tuple_element_addr" "struct" "struct_extract"
                    "struct_element_addr" "ref_element_addr" "ref_tail_addr"
                    "autorelease_value" "copy_value" "destroy_value"
                    "unmanaged_retain_value" "unmanaged_release_value"
                    "unmanaged_autorelease_value"
                    "strong_copy_unowned_value" "strong_copy_unmanaged_value"
                    "destructure_struct" "destructure_tuple" "move_value"
                    "explicit_copy_value")
                  'words) . font-lock-keyword-face)
   ;; Enums. *NOTE* We do not include enum itself here since enum is a
   ;; swift declaration as well handled at the top.
   `(,(regexp-opt '("init_enum_data_addr" "unchecked_enum_data"
                    "unchecked_take_enum_data_addr" "inject_enum_addr"
                    "select_enum" "select_value" "select_enum_addr")
                  'words) . font-lock-keyword-face)
   ;; Protocol and Protocol Composition Types
   `(,(regexp-opt '("init_existential_addr" "deinit_existential_addr"
                    "open_existential_addr"
                    "init_existential_value" "deinit_existential_value"
                    "open_existential_value" "open_existential_box_value"
                    "alloc_existential_box" "project_existential_box"
                    "open_existential_box" "dealloc_existential_box"
                    "init_existential_ref" "open_existential_ref"
                    "open_existential_metatype"
                    "objc_protocol")
                  'words) . font-lock-keyword-face)
   ;; Unchecked Conversions
   `(,(regexp-opt '("upcast"
                    "address_to_pointer" "pointer_to_address"
                    "unchecked_addr_cast"
                    "unchecked_ref_cast"
                    "unchecked_trivial_bit_cast"
                    "unchecked_bitwise_cast"
                    "unchecked_value_cast"
                    "ref_to_raw_pointer" "raw_pointer_to_ref"
                    "unowned_to_ref" "ref_to_unowned"
                    "convert_function" "convert_escape_to_noescape"
                    "ref_to_unmanaged" "unmanaged_to_ref"
                    "ref_to_bridge_object"
                    "bridge_object_to_word" "bridge_object_to_ref"
                    "thin_to_thick_function"
                    "thick_to_objc_metatype" "objc_to_thick_metatype"
                    "objc_metatype_to_object"
                    "objc_existential_metatype_to_object"
                    "word_to_bridge_object"

                    )
                  'words) . font-lock-keyword-face)

   ;; Checked Conversions
   `(,(regexp-opt '("unconditional_checked_cast" "unconditional_checked_cast_addr"
                    "unconditional_checked_cast_value")
                  'words) . font-lock-keyword-face)
   ;; Runtime Failures
   `(,(regexp-opt '("cond_fail")
                  'words) . font-lock-keyword-face)
   ;; Terminators
   `(,(regexp-opt '("unreachable" "return" "br"
                    "cond_br" "switch_value" "switch_enum"
                    "switch_enum_addr" "dynamic_method_br"
                    "checked_cast_br" "checked_cast_value_br" "throw" "checked_cast_addr_br" "case")
                  'words) . font-lock-keyword-face)
   ;; Blocks
   `(,(regexp-opt '("project_block_storage" "init_block_storage_header"
                    "copy_block")
                  'words) . font-lock-keyword-face)
   ;; Debug Info
   `(,(regexp-opt '("loc" "scope" "parent" "inlined_at")
                  'words) . font-lock-keyword-face)
   ;; noimplicit copy
   `(,(regexp-opt '("moveonlywrapper_to_copyable" "moveonlywrapper_to_copyable_addr"
                    "copyable_to_moveonlywrapper" "copyable_to_moveonlywrapper_addr"
                    "moveonlywrapper_to_copyable_box")
                  'words) . font-lock-keyword-face)
   ;; SIL Value
   '("\\b[%][A-Za-z_0-9]+\\([#][0-9]+\\)?\\b" . font-lock-variable-name-face)
   ;; Variables
   '("[a-zA-Z_][a-zA-Z_0-9]*" . font-lock-variable-name-face)
   ;; Unnamed variables
   '("$[0-9]+" . font-lock-variable-name-face)

   )
  "Syntax highlighting for SIL"
  )

;; ---------------------- Syntax table ---------------------------

(unless sil-mode-syntax-table
    (progn
      (setq sil-mode-syntax-table (make-syntax-table))
      (mapc (function (lambda (n)
                          (modify-syntax-entry (aref n 0)
                                               (aref n 1)
                                               sil-mode-syntax-table)))
              '(
                ;; whitespace (` ')
                [?\f  " "]
                [?\t  " "]
                [?\   " "]
                ;; word constituents (`w')
                ;; comments
                [?/  ". 124"]
                [?*  ". 23b"]
                [?\n  ">"]
                [?\^m  ">"]
                ;; symbol constituents (`_')
                [?_ "w"]
                ;; punctuation (`.')
                ;; open paren (`(')
                [?\( "())"]
                [?\[ "(]"]
                [?\{ "(}"]
                ;; close paren (`)')
                [?\) ")("]
                [?\] ")["]
                [?\} "){"]
                ;; string quote ('"')
                [?\" "\""]
                ;; escape-syntax characters ('\\')
                [?\\ "\\"]
                ;; character quote ('"')
                [?\' "\""]
                ))))

;; --------------------- Abbrev table -----------------------------

(defvar sil-mode-abbrev-table nil
  "Abbrev table used while in SIL mode.")
(define-abbrev-table 'sil-mode-abbrev-table ())

(defvar sil-mode-hook nil)
(defvar sil-mode-map nil)   ;; Create a mode-specific keymap.

(unless sil-mode-map
  (setq sil-mode-map (make-sparse-keymap))
  (define-key sil-mode-map "\t" 'tab-to-tab-stop)
  (define-key sil-mode-map "\es" 'center-line)
  (define-key sil-mode-map "\eS" 'center-paragraph))

;;; Helper functions

;; ViewCFG Integration
;;
;; *NOTE* viewcfg must be in the $PATH and .dot files should be associated with
;; the graphviz app.
(defvar sil-mode-viewcfg-program-name "viewcfg")
(defvar sil-mode-viewcfg-renderer "dot")
(defvar sil-mode-viewcfg-buffer-name "*viewcfg*")

(defcustom sil-mode-viewcfg-command-default "viewcfg"
  "The path to the viewcfg command that should be used to dump
  partial cfgs if we can not find viewcfg locally ourselves using swift-project-settings")
;; TODO: If we have swift-project-settings enabled, we will know the swift
;; source root directory. This will let us just use the absolute path to
;; viewcfg.
(defun get-viewcfg-command() sil-mode-viewcfg-command-default)

(defvar sil-mode-viewcfg-command (get-viewcfg-command)
  "The path to the viewcfg command that should be used")

(defun sil-mode-display-function-cfg()
  (interactive)
  ;; First we need to find the previous '{' and then the next '}'.
  (save-mark-and-excursion
    (let ((brace-start (re-search-backward "{\s*$"))
          (brace-end (re-search-forward "^} // end sil function '" nil t))
          (process-connection-type nil))
      ;; See if we failed to find } // end sil function. If we did, search again
      ;; for ^} itself and see if we find anything.
      (if (null brace-end)
          (setq brace-end (re-search-forward "^}")))
      (let ((p (start-process sil-mode-viewcfg-program-name
                              sil-mode-viewcfg-buffer-name
                              sil-mode-viewcfg-command
                              (concat "--renderer=" sil-mode-viewcfg-renderer))))
        (process-send-region p brace-start brace-end)
       (process-send-eof p)))))

;;; Top Level Entry point

(defun sil-mode ()
  "Major mode for editing SIL source files.
  \\{sil-mode-map}
  Runs sil-mode-hook on startup."
  (interactive)
  (kill-all-local-variables)
  (use-local-map sil-mode-map)         ;; Provides the local keymap.
  (setq major-mode 'sil-mode)

  (make-local-variable 'font-lock-defaults)
  (setq major-mode 'sil-mode           ;; This is how describe-mode
                                       ;; finds the doc string to print.
  mode-name "SIL"                      ;; This name goes into the modeline.
  font-lock-defaults `(sil-font-lock-keywords))

  (setq local-abbrev-table sil-mode-abbrev-table)
  (set-syntax-table sil-mode-syntax-table)
  (setq comment-start "//")
  (setq tab-stop-list (number-sequence 2 120 2))
  (setq tab-width 2)
  (run-hooks 'sil-mode-hook))          ;; Finally, this permits the user to
                                       ;;   customize the mode with a hook.

;; Associate .sil files with sil-mode
(setq auto-mode-alist
   (append '(("\\.sil$" . sil-mode)) auto-mode-alist))

(provide 'sil-mode)
;; end of sil-mode.el
