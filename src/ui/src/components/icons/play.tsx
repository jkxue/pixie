import * as React from 'react';

import SvgIcon, {SvgIconProps} from '@material-ui/core/SvgIcon';

const PlayIcon = (props: SvgIconProps) => (
  <SvgIcon {...props} viewBox='0 0 20 22'>
    <path fillRule='evenodd' clipRule='evenodd'
      d={`M18.5 8.40205C20.5 9.55675 20.5 12.4435 18.5 13.5982L5 21.3924C3 22.5471 0.500001
      21.1037 0.500001 18.7943L0.500002 3.20589C0.500002 0.896489
      3 -0.546886 5 0.607815L18.5 8.40205ZM17.5 11.8661C18.1667 11.4812
      18.1667 10.519 17.5 10.1341L4 2.33987C3.33334 1.95497
      2.5 2.43609 2.5 3.20589L2.5 18.7943C2.5 19.5641 3.33334 20.0453 4 19.6604L17.5 11.8661Z`} />
  </SvgIcon>
);

export default PlayIcon;
