// Author: Markus Schordan, 2013.

#include "sage3basic.h"

#include <iostream>

#include "AstAnnotator.h"
#include "RoseAst.h"
#include <iostream>
using namespace std;

AstAnnotator::AstAnnotator(Labeler* labeler):_labeler(labeler) {
}

void AstAnnotator::annotateAstAttributesAsCommentsBeforeStatements(SgNode* node, string attributeName) {
  annotateAstAttributesAsComments(node,attributeName,PreprocessingInfo::before);
}
void AstAnnotator::annotateAstAttributesAsCommentsAfterStatements(SgNode* node, string attributeName) {
  annotateAstAttributesAsComments(node,attributeName,PreprocessingInfo::after);
}

void AstAnnotator::annotateAstAttributesAsComments(SgNode* node, string attributeName,PreprocessingInfo::RelativePositionType posSpecifier) {
  RoseAst ast(node);
  for(RoseAst::iterator i=ast.begin(); i!=ast.end();++i) {
    if(SgStatement* stmt=dynamic_cast<SgStatement*>(*i)) {
      if(isSgCtorInitializerList(*i)) {
        //std::cerr << "WARNING: attaching comments to AST nodes of type SgCtorInitializerList not possible. We are skipping this annotation and continue."<<std::endl;
        continue;
      }
      DFAstAttribute* artAttribute=dynamic_cast<DFAstAttribute*>(stmt->getAttribute(attributeName));
      // if this fails, no attribute was attached
      if(artAttribute) {
        ROSE_ASSERT(_labeler);
        //cout << "@"<<stmt<<" "<<stmt->class_name()<<" FOUND LABEL: "<<_labeler->getLabel(stmt)<<endl;
        string labelString=_labeler->labelToString(_labeler->getLabel(stmt));
        string commentStart="// ";
        insertComment(commentStart+labelString+": "+artAttribute->toString(),posSpecifier,stmt);
      }
    }
  }
}

// posSpecifier: PreprocessingInfo::before, PreprocessingInfo::after
void AstAnnotator::insertComment(std::string comment, PreprocessingInfo::RelativePositionType posSpecifier, SgStatement* node) {
  assert(posSpecifier==PreprocessingInfo::before || posSpecifier==PreprocessingInfo::after);
  PreprocessingInfo* commentInfo = 
    new PreprocessingInfo(PreprocessingInfo::CplusplusStyleComment, 
                          comment,
                          "user-generated",0, 0, 0, 
                          posSpecifier // e.g. PreprocessingInfo::before
                          );
  node->addToAttachedPreprocessingInfo(commentInfo);
}

