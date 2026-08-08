

void serialize(PrettyInputArchive& ar1, VARIANT_NAME& v) {
  string name;
  auto bookmark = ar1.bookmark();
  ar1.readText(name);
#define X(Type, Index)\
  if (name == #Type) { \
    v.index = Index; \
    new(&v.elem##Index) Type;\
    ar1(v.elem##Index); \
  } else
  VARIANT_TYPES_LIST
#undef X
#ifdef DEFAULT_ELEM
// The guard has to live INSIDE the branch: this whole thing is one if/else-if chain hanging off the
// type-name tests above, so a statement between them would attach to the previous `else`.
#define X(Type, Index)\
  if (!strcmp(DEFAULT_ELEM, #Type)) { \
    /* Only fall back if this exact position is not already being parsed as a default elem -- otherwise an \
       unknown type name recurses forever instead of erroring (see enterDefaultElem). */ \
    if (!ar1.enterDefaultElem(bookmark)) \
      ar1.error(name + " is not part of variant"); \
    v.index = Index; \
    new(&v.elem##Index) Type;\
    ar1.seek(bookmark);\
    ar1(v.elem##Index); \
    ar1.leaveDefaultElem(); \
  } else
  VARIANT_TYPES_LIST
  #undef X
  ar1.error("Bad default elem");
#else
  ar1.error(name + " is not part of variant");
#endif
}
