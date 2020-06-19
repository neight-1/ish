//
//  Roots.h
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@protocol ProgressReporter

- (void)updateProgress:(double)progressFraction message:(NSString *)progressMessage;

@end

@interface Roots : NSObject

+ (instancetype)instance;

@property (readonly) NSOrderedSet<NSString *> *roots;
@property NSString *defaultRoot;
- (BOOL)importRootFromArchive:(NSURL *)archive name:(NSString *)name error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress;
- (BOOL)exportRootNamed:(NSString *)name toArchive:(NSURL *)archive error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress;
- (BOOL)destroyRootNamed:(NSString *)name error:(NSError **)error;

@end

NS_ASSUME_NONNULL_END
