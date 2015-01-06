<!DOCTYPE style-sheet PUBLIC "-//James Clark//DTD DSSSL Style Sheet//EN" [
<!ENTITY docbook.dsl PUBLIC "-//Norman Walsh//DOCUMENT DocBook HTML Stylesheet//EN" CDATA dsssl>
]>
<style-sheet>
<style-specification id="html" use="docbook">
<style-specification-body>

(define %chapter-autolabel% #t)
(define %section-autolabel% #t)
(define (toc-depth nd) 3)

; (declare-characteristic preserve-sdata?
;           "UNREGISTERED::James Clark//Characteristic::preserve-sdata?"
;           #f)

(define %root-filename% "index")        ;; name for the root html file

;;Use element ids as filenames?
(define %use-id-as-filename%
 #t)

;;Default extension for filenames?
(define %html-ext%
  ".html")

; === Rendering ===
(define %admon-graphics% #t)            ;; use symbols for Caution|Important|Note|Tip|Warning

; === Books only ===
(define %generate-book-titlepage% #t)
(define %generate-book-toc% #t)
(define ($generate-chapter-toc$) #f)    ;; never generate a chapter TOC in books

; === HTML settings ===
(define %html-pubid% "-//W3C//DTD HTML 4.01 Transitional//EN") ;; Nearly true :-(
(define %html40% #t)

</style-specification-body>
</style-specification>
<external-specification id="docbook" document="docbook.dsl">
</style-sheet>
