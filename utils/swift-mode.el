;===--- swift-mode.el ----------------------------------------------------===;
;
; This source file is part of the Swift.org open source project
;
; Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
; Licensed under Apache License v2.0 with Runtime Library Exception
;
; See https://swift.org/LICENSE.txt for license information
; See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
;
;===----------------------------------------------------------------------===;

(require 'compile)
(unless (fboundp 'prog-mode)
  (define-derived-mode prog-mode fundamental-mode "Prog"
    "Base mode for other programming language modes"
    (setq bidi-paragraph-direction 'left-to-right)
    (set
     (make-local-variable 'require-final-newline) mode-require-final-newline)
    (set
     (make-local-variable 'parse-sexp-ignore-comments) t)))

(unless (fboundp 'defvar-local)
  (defmacro defvar-local (var val &optional docstring)
    "Define VAR as a buffer-local variable with default value VAL."
    `(make-variable-buffer-local (defvar ,var ,val ,docstring))))

;; Create mode-specific variables
(defcustom swift-basic-offset 2
  "Default indentation width for Swift source"
  :type 'integer)


;; Create mode-specific tables.
(defvar swift-mode-syntax-table nil
  "Syntax table used while in SWIFT mode.")

(defvar swift-font-lock-keywords
  (list
   ;; Comments
   '("^#!.*" . font-lock-comment-face)
   ;; Variables surrounded with backticks (`)
   '("`[a-zA-Z_][a-zA-Z_0-9]*`" . font-lock-variable-name-face)
   ;; Types
   '("\\b[A-Z][a-zA-Z_0-9]*\\b" . font-lock-type-face)
   ;; Floating point constants
   '("\\b[-+]?[0-9]+\.[0-9]+\\b" . font-lock-preprocessor-face)
   ;; Integer literals
   '("\\b[-]?[0-9]+\\b" . font-lock-preprocessor-face)
   ;; Decl and type keywords
   `(,(regexp-opt '("import"
                    "class" "struct" "enum" "extension" "protocol" "typealias" "var" "let" "actor"
                    "func" "init" "deinit" "subscript" "associatedtype"
                    "public" "internal" "private" "fileprivate" "package"
                    "static"
                    "where")
                  'words) . font-lock-keyword-face)
   ;; Variable decl keywords
   `("\\b\\(?:[^a-zA-Z_0-9]*\\)\\(get\\|set\\|_read\\|_modify\\|unsafe\\(Mutable\\)?Address\\)\\(?:[^a-zA-Z_0-9]*\\)\\b" 1 font-lock-keyword-face)
   `(,(regexp-opt '("willSet" "didSet") 'words) . font-lock-keyword-face)
   ;; Operators
   `("\\b\\(?:\\(?:pre\\|post\\|in\\)fix\\s-+\\)operator\\b" . font-lock-keyword-face)
   ;; Keywords that begin with a number sign
   `("#\\(if\\|endif\\|elseif\\|else\\|available\\|error\\|warning\\)\\b" . font-lock-string-face)
   `("#\\(file\\|line\\|column\\|function\\|selector\\)\\b" . font-lock-keyword-face)
   ;; Infix operator attributes
   `(,(regexp-opt '("precedence" "associativity" "left" "right" "none" "precedencegroup")
                  'words) . font-lock-keyword-face)
   ;; Statements
   `(,(regexp-opt '("if" "guard" "in" "else" "for" "do" "repeat" "while"
                    "return" "break" "continue" "fallthrough"  "switch" "case"
                    "default" "defer" "catch" "yield")
                  'words) . font-lock-keyword-face)
   ;; Decl modifier keywords
   `(,(regexp-opt '("mutating" "nonmutating" "__consuming" "consuming" "borrowing" "inout"
                    "convenience" "dynamic" "optional"
                    "indirect" "override" "open" "final"
                    "required" "lazy" "weak"
                    "_compilerInitialized" "_const" "_local"
                    "nonisolated" "distributed")
                  'words) . font-lock-keyword-face)
   `("\\<unowned\\((\\(un\\)?safe)\\)?\\>" . font-lock-keyword-face)
   ;; Expression keywords: "Any" and "Self" are included in "Types" above
   `(,(regexp-opt '("as" "false" "is" "nil" "rethrows" "super" "self" "throw"
                    "true" "try" "throws" "async" "await"
                    "consume" "copy" "_move" "_borrow" "discard"
                    "any" "some" "repeat" "each")
                  'words) . font-lock-keyword-face)
   ;; Expressions
   `(,(regexp-opt '("new") 'words) . font-lock-keyword-face)
   ;; Variables
   '("[a-zA-Z_][a-zA-Z_0-9]*" . font-lock-variable-name-face)
   ;; Unnamed variables
   '("$[0-9]+" . font-lock-variable-name-face)
   )
  "Syntax highlighting for SWIFT"
  )

;; ---------------------- Syntax table ---------------------------

(if (not swift-mode-syntax-table)
    (progn
      (setq swift-mode-syntax-table (make-syntax-table))
      (mapc (function (lambda (n)
                        (modify-syntax-entry (aref n 0)
                                             (aref n 1)
                                             swift-mode-syntax-table)))
            '(
              ;; whitespace (` ')
              [?\f  " "]
              [?\t  " "]
              [?\   " "]
              ;; word constituents (`w')
              ;; punctuation
              [?< "."]
              [?> "."]
              ;; comments
              [?/  ". 124"]
              [?*  ". 23b"]
              [?\n  ">"]
              [?\^m  ">"]
              ;; symbol constituents (`_')
              [?_ "_"]
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
              ))))

;; --------------------- Abbrev table -----------------------------

(defvar swift-mode-abbrev-table nil
  "Abbrev table used while in SWIFT mode.")
(define-abbrev-table 'swift-mode-abbrev-table ())

(defvar swift-mode-map
  (let ((keymap (make-sparse-keymap)))
    keymap)
  "Keymap for `swift-mode'.")


;;;###autoload
(define-derived-mode swift-mode prog-mode "Swift"
  "Major mode for editing SWIFT source files.
  \\{swift-mode-map}
  Runs swift-mode-hook on startup."
  :group 'swift

  (require 'electric)
  (set (make-local-variable 'indent-line-function) 'swift-indent-line)
  (set (make-local-variable 'parse-sexp-ignore-comments) t)
  (set (make-local-variable 'comment-use-syntax) nil) ;; don't use the syntax table; use our regexp
  (set (make-local-variable 'comment-start-skip) "\\(?:/\\)\\(?:/[:/]?\\|[*]+\\)[ \t]*")
  (set (make-local-variable 'comment-start) "// ")
  (set (make-local-variable 'comment-end) "")

  (unless (boundp 'electric-indent-chars)
    (defvar electric-indent-chars nil))
  (unless (boundp 'electric-pair-pairs)
    (defvar electric-pair-pairs nil))

  (set (make-local-variable 'electric-indent-chars)
       (append "{}()[]:," electric-indent-chars))
  (set (make-local-variable 'electric-pair-pairs)
       (append '(
                 ;; (?' . ?\') ;; This isn't such a great idea because
                 ;; pairs are detected even in strings and comments,
                 ;; and sometimes an apostrophe is just an apostrophe
                 (?{ . ?})  (?[ . ?]) (?( . ?)) (?` . ?`)) electric-pair-pairs))
  (set (make-local-variable 'electric-layout-rules)
       '((?\{ . after) (?\} . before)))

  (set (make-local-variable 'font-lock-defaults)
       '(swift-font-lock-keywords) ))

(defconst swift-doc-comment-detail-re
  (let* ((just-space "[ \t\n]*")
        (not-just-space "[ \t]*[^ \t\n].*")
        (eol "\\(?:$\\)")
        (continue "\n\\1"))

    (concat "^\\([ \t]*///\\)" not-just-space eol
            "\\(?:" continue not-just-space eol "\\)*"
            "\\(" continue just-space eol
            "\\(?:" continue ".*" eol "\\)*"
            "\\)"))
  "regexp that finds the non-summary part of a swift doc comment as subexpression 2")

(defun swift-hide-doc-comment-detail ()
  "Hide everything but the summary part of doc comments.

Use `M-x hs-show-all' to show them again."
    (interactive)
  (hs-minor-mode)
  (save-excursion
    (save-match-data
      (goto-char (point-min))
      (while (search-forward-regexp swift-doc-comment-detail-re (point-max) :noerror)
        (hs-hide-comment-region (match-beginning 2) (match-end 2))
        (goto-char (match-end 2))))))

(defvar swift-mode-generic-parameter-list-syntax-table
  (let ((s (copy-syntax-table swift-mode-syntax-table)))
    (modify-syntax-entry ?\< "(>" s)
    (modify-syntax-entry ?\> ")<" s)
    s))

(defun swift-skip-comments-and-space ()
  "Skip comments and whitespace, returning t"
  (while (forward-comment 1))
  t)

(defconst swift-identifier-re "\\_<[[:alpha:]_].*?\\_>")

(defun swift-skip-optionality ()
  "Hop over any comments, whitespace, and strings
of `!' or `?', returning t unconditionally."
  (swift-skip-comments-and-space)
  (while (not (zerop (skip-chars-forward "!?")))
    (swift-skip-comments-and-space)))

(defun swift-skip-generic-parameter-list ()
  "Hop over any comments, whitespace, and, if present, a generic
parameter list, returning t if the parameter list was found and
nil otherwise."
  (swift-skip-comments-and-space)
  (when (looking-at "<")
    (with-syntax-table swift-mode-generic-parameter-list-syntax-table
      (ignore-errors (forward-sexp) t))))

(defun swift-skip-re (pattern)
  "Hop over any comments and whitespace; then if PATTERN matches
the next characters skip over them, returning t if so and nil
otherwise."
  (swift-skip-comments-and-space)
  (save-match-data
    (when (looking-at pattern)
      (goto-char (match-end 0))
      t)))

(defun swift-skip-identifier ()
  "Hop over any comments, whitespace, and an identifier if one is
present, returning t if so and nil otherwise."
  (swift-skip-re swift-identifier-re))

(defun swift-skip-simple-type-name ()
  "Hop over a chain of the form identifier
generic-parameter-list? ( `.' identifier generic-parameter-list?
)*, returning t if the initial identifier was found and nil otherwise."
  (when (swift-skip-identifier)
    (swift-skip-generic-parameter-list)
    (when (swift-skip-re "\\.")
      (swift-skip-simple-type-name))
    t))

(defun swift-skip-type-name ()
    "Hop over any comments, whitespace, and the name of a type if
one is present, returning t if so and nil otherwise"
  (swift-skip-comments-and-space)
  (let ((found nil))
    ;; repeatedly
    (while
        (and
         ;; match a tuple or an identifier + optional generic param list
         (cond
          ((looking-at "[[(]")
           (forward-sexp)
           (setq found t))

          ((swift-skip-simple-type-name)
           (setq found t)))

          ;; followed by "->"
         (prog2 (swift-skip-re "\\?+")
             (swift-skip-re "throws\\|rethrows\\|->")
           (swift-skip-re "->") ;; accounts for the throws/rethrows cases on the previous line
           (swift-skip-comments-and-space))))
    found))

(defun swift-skip-constraint ()
    "Hop over a single type constraint if one is present,
returning t if so and nil otherwise"
  (swift-skip-comments-and-space)
  (and (swift-skip-type-name)
       (swift-skip-re ":\\|==")
       (swift-skip-type-name)))

(defun swift-skip-where-clause ()
    "Hop over a where clause if one is present, returning t if so
and nil otherwise"
  (when (swift-skip-re "\\<where\\>")
    (while (and (swift-skip-constraint) (swift-skip-re ",")))
    t))

(defun swift-in-string-or-comment ()
  "Return non-nil if point is in a string or comment."
  (or (nth 3 (syntax-ppss)) (nth 4 (syntax-ppss))))

(defconst swift-body-keyword-re
  "\\_<\\(var\\|func\\|init\\|deinit\\|subscript\\)\\_>")

(defun swift-hide-bodies ()
  "Hide the bodies of methods, functions, and computed properties.

Use `M-x hs-show-all' to show them again."
    (interactive)
  (hs-minor-mode)
  (save-excursion
    (save-match-data
      (goto-char (point-min))
      (while (search-forward-regexp swift-body-keyword-re (point-max) :noerror)
        (when
            (and
             (not (swift-in-string-or-comment))
             (let ((keyword (match-string 0)))
               ;; parse up to the opening brace
               (cond
                ((equal keyword "deinit") t)

                ((equal keyword "var")
                 (and (swift-skip-identifier)
                      (swift-skip-re ":")
                      (swift-skip-type-name)))

                ;; otherwise, there's a parameter list
                (t
                 (and
                  ;; parse the function's base name or operator symbol
                  (if (equal keyword "func") (forward-symbol 1) t)
                  ;; advance to the beginning of the function
                  ;; parameter list
                  (progn
                    (swift-skip-generic-parameter-list)
                    (swift-skip-comments-and-space)
                    (equal (char-after) ?\())
                  ;; parse the parameter list and any return type
                  (prog1
                    (swift-skip-type-name)
                    (swift-skip-where-clause))))))
             (swift-skip-re "{"))
          (hs-hide-block :reposition-at-end))))))

(defun swift-indent-line ()
  (interactive)
  (let (indent-level target-column)
    (save-excursion
      (widen)
      (setq indent-level (car (syntax-ppss (point-at-bol))))

      ;; Look at the first non-whitespace to see if it's a close paren
      (beginning-of-line)
      (skip-syntax-forward " ")
      (setq target-column
            (if (or (equal (char-after) ?\#) (looking-at "//:")) 0
              (* swift-basic-offset
                 (- indent-level
                    (cond ((= (char-syntax (or (char-after) ?\X)) ?\))
                           1)
                          ((save-match-data
                             (looking-at
                              "case \\|default *:\\|[a-zA-Z_][a-zA-Z0-9_]*\\(\\s-\\|\n\\)*:\\(\\s-\\|\n\\)*\\(for\\|do\\|\\while\\|switch\\|repeat\\)\\>"))
                           1)
                          (t 0))))))
      (indent-line-to (max target-column 0)))
    (when (< (current-column) target-column)
      (move-to-column target-column)))
  )

;; Compilation error parsing
(push 'swift0 compilation-error-regexp-alist)
(push 'swift1 compilation-error-regexp-alist)
(push 'swift-fatal compilation-error-regexp-alist)

(push `(swift0
        ,(concat
     "^"
       "[ \t]+" "\\(?:(@\\)?"
       "[A-Z⚠️][A-Za-z0-9_]*@"
     ;; Filename \1
       "\\("
          "[0-9]*[^0-9\n]"
          "\\(?:"
             "[^\n :]" "\\|" " [^/\n]" "\\|" ":[^ \n]"
          "\\)*?"
       "\\)"
       ":"
       ;; Line number (\2)
       "\\([0-9]+\\)"
       ":"
       ;; Column \3
       "\\([0-9]+\\)"
       )
     1 2 3 0)
      compilation-error-regexp-alist-alist)

(push `(swift1
        ,(concat
     "^"
       "[0-9]+[.][ \t]+While .* at \\[?"
     ;; Filename \1
       "\\("
          "[0-9]*[^0-9\n]"
          "\\(?:"
             "[^\n :]" "\\|" " [^/\n]" "\\|" ":[^ \n]"
          "\\)*?"
       "\\)"
       ":"
       ;; Line number (\2)
       "\\([0-9]+\\)"
       ":"
       ;; Column \3
       "\\([0-9]+\\)"
       )
     1 2 3 2)
      compilation-error-regexp-alist-alist)

(push `(swift-fatal
        ,(concat
     "^\\(?:assertion failed\\|fatal error\\): \\(?:.*: \\)?file "
     ;; Filename \1
       "\\("
          "[0-9]*[^0-9\n]"
          "\\(?:"
             "[^\n :]" "\\|" " [^/\n]" "\\|" ":[^ \n]"
          "\\)*?"
       "\\)"
       ", line "
       ;; Line number (\2)
       "\\([0-9]+\\)"
       )
     1 2 nil 2)
      compilation-error-regexp-alist-alist)

;; Flymake support

(require 'flymake)

;; This name doesn't end in "function" to avoid being unconditionally marked as risky.
(defvar-local swift-find-executable-fn 'executable-find
  "Function to find a command executable.
The function is called with one argument, the name of the executable to find.
Might be useful if you want to use a swiftc that you built instead
of the one in your PATH.")
(put 'swift-find-executable-fn 'safe-local-variable 'functionp)

(defvar-local swift-syntax-check-fn 'swift-syntax-check-directory
"Function to create the swift command-line that syntax-checks the current buffer.
The function is called with two arguments, the swiftc executable, and
the name of a temporary file that will contain the contents of the
current buffer.
Set to 'swift-syntax-check-single-file to ignore other files in the current directory.")
(put 'swift-syntax-check-fn 'safe-local-variable 'functionp)

(defvar-local swift-syntax-check-args '("-typecheck")
  "List of arguments to be passed to swiftc for syntax checking.
Elements of this list that are strings are inserted literally
into the command line.  Elements that are S-expressions are
evaluated.  The resulting list is cached in a file-local
variable, `swift-syntax-check-evaluated-args', so if you change
this variable you should set that one to nil.")
(put 'swift-syntax-check-args 'safe-local-variable 'listp)

(defvar-local swift-syntax-check-evaluated-args
  "File-local cache of swift arguments used for syntax checking
variable, `swift-syntax-check-args', so if you change
that variable you should set this one to nil.")

(defun swift-syntax-check-single-file (swiftc temp-file)
  "Return a flymake command-line list for syntax-checking the current buffer in isolation"
  `(,swiftc ("-typecheck" ,temp-file)))

(defun swift-syntax-check-directory (swiftc temp-file)
  "Return a flymake command-line list for syntax-checking the
current buffer along with the other swift files in the same
directory."
  (let* ((sources nil))
    (dolist (x (directory-files (file-name-directory (buffer-file-name))))
      (when (and (string-equal "swift" (file-name-extension x))
                 (not (file-equal-p x (buffer-file-name))))
        (setq sources (cons x sources))))
    `(,swiftc ("-typecheck" ,temp-file ,@sources))))

(defun flymake-swift-init ()
  (let* ((temp-file
          (flymake-init-create-temp-buffer-copy
           (lambda (x y)
             (make-temp-file
              (concat (file-name-nondirectory x) "-" y)
              (not :DIR_FLAG)
              ;; grab *all* the extensions; handles .swift.gyb files, for example
              ;; whereas using file-name-extension would only get ".gyb"
              (replace-regexp-in-string "^\\(?:.*/\\)?[^.]*" "" (buffer-file-name)))))))
    (funcall swift-syntax-check-fn
             (funcall swift-find-executable-fn "swiftc")
             temp-file)))

(add-to-list 'flymake-allowed-file-name-masks '(".+\\.swift$" flymake-swift-init))

(setq flymake-err-line-patterns
      (append
       (flymake-reformat-err-line-patterns-from-compile-el
        (mapcar (lambda (x) (assoc x compilation-error-regexp-alist-alist))
                '(swift0 swift1 swift-fatal)))
       flymake-err-line-patterns))

(defgroup swift nil
  "Major mode for editing swift source files."
  :prefix "swift-")

(provide 'swift-mode)

;; end of swift-mode.el
